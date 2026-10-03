// Self-updating development builds; see devupdate.h.
//
// HTTP/1.1 over plain sockets (not httpc, which gives up on a Wi-Fi stall), one connection kept
// open, every receive bounded by a timeout. Data files come in packs (POST /dev/pack: many files
// in one stream, no round trip per file), each checked against the manifest's CRC32; a pack cut
// short loses only the file in flight. The code CIA downloads with Range resume.
#include "devupdate.h"

#include <3ds.h>
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <deque>
#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <netinet/in.h>
#include <poll.h>
#include <set>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <zlib.h>

#include "log.h"

static const char* kDataDir = "sdmc:/3ds/mw3ds/data";
static const char* kManifest = "sdmc:/3ds/mw3ds/data/.manifest";
static const char* kCodeCia = "sdmc:/3ds/mw3ds/update.cia";
static const char* kTrash = "/3ds/mw3ds/trash";          // (SD archive path) old data waiting to be deleted
static const int kTimeoutMs = 60000;         // no data for this long: reconnect (3DS Wi-Fi stalls 10-20 s)
static const int kProbeTimeoutMs = 2500;     // the first contact: an absent PC costs this much
static const int kAttempts = 8;
static const size_t kPackFiles = 200;        // per pack: what a dropped connection can cost
static const long long kPackBytes = 4 << 20;

static std::string s_host;
static int s_port = 0;
static int s_timeoutMs = kTimeoutMs;
static int s_conn = -1;                      // kept open across requests
static bool s_stop = false;                  // B held: stop updating and play
static bool s_quit = false;                  // closed from the HOME menu

// Lets the system run (HOME menu, power) and reads B, at most every 100 ms; true to stop
static bool userStops()
{
	static u64 last = 0;
	u64 now = osGetTime();
	if (now - last >= 100)
	{
		last = now;
		if (!aptMainLoop())
			s_quit = true;
		hidScanInput();
		if (hidKeysHeld() & KEY_B)
			s_stop = true;
	}
	return s_stop || s_quit;
}

typedef std::function<void(const std::string&, const std::string&)> Progress;

// ---- Sockets

static int connectHost()
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return -1;
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_port = htons(s_port);
	inet_aton(s_host.c_str(), &a.sin_addr);
	fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
	int r = connect(s, (sockaddr*)&a, sizeof(a));
	// (the emulator reports a pending connect as EALREADY / EWOULDBLOCK rather than EINPROGRESS)
	if (r < 0 && errno != EINPROGRESS && errno != EALREADY && errno != EWOULDBLOCK)
	{
		close(s);
		return -1;
	}
	if (r < 0)
	{
		pollfd p = { s, POLLOUT, 0 };
		int pr = poll(&p, 1, std::min(s_timeoutMs, 4000));
		if (pr <= 0 || (p.revents & (POLLERR | POLLHUP)))
		{
			close(s);
			return -1;
		}
	}
	return s;
}

static void dropConnection()
{
	if (s_conn >= 0)
		close(s_conn);
	s_conn = -1;
}

// Receive up to n bytes, waiting at most the timeout; 0 = closed, -1 = error / timeout
// Receive statistics per pack (logged): calls, bytes, the longest wait for data
static u32 s_recvCalls = 0;
static long long s_recvBytes = 0;
static u64 s_maxWaitMs = 0;

static int recvSome(char* buf, int n)
{
	// Wait in short slices so B / HOME still work through a stall
	u64 start = osGetTime();
	for (;;)
	{
		pollfd p = { s_conn, POLLIN, 0 };
		int pr = poll(&p, 1, 250);
		if (pr > 0)
			break;
		if (pr < 0 || userStops() || osGetTime() - start >= (u64)s_timeoutMs)
			return -1;
	}
	u64 waited = osGetTime() - start;
	s_maxWaitMs = waited > s_maxWaitMs ? waited : s_maxWaitMs;
	int r = recv(s_conn, buf, n, 0);
	if (r > 0)
	{
		s_recvCalls++;
		s_recvBytes += r;
	}
	return r < 0 && errno == EAGAIN ? -1 : r;
}

static bool sendAll(const std::string& data)
{
	size_t sent = 0;
	while (sent < data.size())
	{
		pollfd p = { s_conn, POLLOUT, 0 };
		if (poll(&p, 1, s_timeoutMs) <= 0)
			return false;
		int r = send(s_conn, data.data() + sent, data.size() - sent, 0);
		if (r <= 0)
			return false;
		sent += r;
	}
	return true;
}

// ---- HTTP

static std::string urlEncode(const std::string& path)
{
	std::string out;
	for (unsigned char c : path)
	{
		if (isalnum(c) || c == '/' || c == '.' || c == '_' || c == '-')
			out += c;
		else
		{
			char hex[4];
			snprintf(hex, sizeof(hex), "%%%02X", c);
			out += hex;
		}
	}
	return out;
}

// Reads a response body through a buffer (what came along with the head first)
struct Body
{
	std::string pending;          // received, not yet handed out: pending[pos..]
	size_t pos = 0;
	long long left = -1;          // body bytes not yet handed out (-1: until the connection closes)
	bool keep = false;            // the connection can carry the next request

	// Up to n bytes: > 0 read, 0 the end, -1 the connection failed or stalled
	int read(char* buf, int n)
	{
		if (left == 0)
			return 0;
		if (left > 0 && n > left)
			n = (int)left;
		if (pos >= pending.size())
		{
			pending.resize(64 * 1024);
			pos = 0;
			long long want = left > 0 ? std::min<long long>(pending.size(), left) : pending.size();
			int r = recvSome(&pending[0], (int)want);
			pending.resize(r > 0 ? r : 0);
			if (r == 0 && left < 0)
				return 0;
			if (r <= 0)
				return -1;
		}
		int r = std::min<int>(n, pending.size() - pos);
		memcpy(buf, pending.data() + pos, r);
		pos += r;
		if (left > 0)
			left -= r;
		return r;
	}
};

// Sends a request and reads the response head; the status, or -1 when the connection failed
static int request(const std::string& method, const std::string& url, const std::string& extraHeaders,
	const std::string& payload, Body& body)
{
	std::string req = method + " " + urlEncode(url) + " HTTP/1.1\r\nHost: " + s_host + "\r\nConnection: keep-alive\r\n"
		+ extraHeaders;
	if (!payload.empty())
		req += "Content-Length: " + std::to_string(payload.size()) + "\r\n";
	req += "\r\n";
	req += payload;
	for (int attempt = 0; attempt < 2; attempt++)
	{
		bool reused = s_conn >= 0;
		if (!reused && (s_conn = connectHost()) < 0)
			return -1;
		std::string head;
		size_t end = std::string::npos;
		bool ok = sendAll(req);
		char buf[4096];
		while (ok && end == std::string::npos)
		{
			int r = recvSome(buf, sizeof(buf));
			if (r <= 0 || head.size() > 65536)
				ok = false;
			else
			{
				head.append(buf, r);
				end = head.find("\r\n\r\n");
			}
		}
		if (!ok)
		{
			dropConnection();
			if (reused)
				continue;             // the kept connection may have timed out on the server: once more
			return -1;
		}
		int status = 0;
		sscanf(head.c_str(), "HTTP/%*s %d", &status);
		std::string lower = head.substr(0, end);
		for (auto& c : lower)
			c = tolower((unsigned char)c);
		size_t cl = lower.find("\r\ncontent-length:");
		body.left = cl != std::string::npos ? atoll(lower.c_str() + cl + 17) : -1;
		body.keep = body.left >= 0 && lower.find("\r\nconnection: close") == std::string::npos;
		body.pending = head.substr(end + 4);
		return status;
	}
	return -1;
}

// Reads the rest of the body (or drops the connection when it can't be reused)
static void finish(Body& body, bool complete)
{
	if (!complete || !body.keep)
		dropConnection();
}

static bool getText(const std::string& url, std::string& out, int attempts = 3)
{
	for (int attempt = 0; attempt < attempts; attempt++)
	{
		out.clear();
		Body body;
		int status = request("GET", url, "", "", body);
		if (status < 0)
			continue;
		char buf[8192];
		int r;
		while ((r = body.read(buf, sizeof(buf))) > 0)
			out.append(buf, r);
		finish(body, r == 0);
		if (status == 200 && r == 0)
			return true;
	}
	return false;
}

static void mkdirs(const std::string& path)
{
	static std::set<std::string> made;             // SD calls are slow: each directory once
	for (size_t i = 1; i < path.size(); i++)
		if (path[i] == '/' && made.insert(path.substr(0, i)).second)
			mkdir(path.substr(0, i).c_str(), 0777);
}

static std::string sizeText(long long bytes)
{
	char buf[32];
	if (bytes >= 1 << 20)
		snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
	else
		snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
	return buf;
}

// Downloads url into path (via path.part, resumed with Range requests); progress(bytes, total)
static bool download(const std::string& url, const std::string& path, const std::function<void(long long, long long)>& progress)
{
	std::string part = path + ".part";
	mkdirs(part);
	for (int attempt = 0; attempt < kAttempts; attempt++)
	{
		struct stat st;
		long long have = stat(part.c_str(), &st) == 0 ? st.st_size : 0;
		Body body;
		int status = request("GET", url, have ? "Range: bytes=" + std::to_string(have) + "-\r\n" : "", "", body);
		if (status == 416)
		{
			finish(body, false);
			remove(part.c_str());
			continue;
		}
		if (status != 200 && status != 206)
		{
			finish(body, false);
			if (status > 0)
			{
				logf("dev: %s: HTTP %d", url.c_str(), status);
				return false;
			}
			svcSleepThread(500000000LL);
			continue;
		}
		if (status == 200)
			have = 0;                                  // the whole file again
		FILE* f = fopen(part.c_str(), have ? "ab" : "wb");
		if (!f)
			return false;
		setvbuf(f, nullptr, _IOFBF, 256 * 1024);         // few, large SD writes
		long long total = body.left >= 0 ? have + body.left : -1, now = have;
		std::vector<char> buf(64 * 1024);
		int r;
		while ((r = body.read(buf.data(), buf.size())) > 0)
		{
			if (userStops())
			{
				fclose(f);
				finish(body, false);
				return false;
			}
			fwrite(buf.data(), 1, r, f);
			now += r;
			if (progress)
				progress(now, total);
		}
		fclose(f);
		finish(body, r == 0);
		if (r == 0)
		{
			remove(path.c_str());
			return rename(part.c_str(), path.c_str()) == 0;
		}
		svcSleepThread(500000000LL);
	}
	return false;
}

// ---- Data sync

struct Entry { long long size; unsigned crc; };

// "size crc path" lines
static std::map<std::string, Entry> parseManifest(const std::string& text)
{
	std::map<std::string, Entry> out;
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t nl = text.find('\n', pos);
		if (nl == std::string::npos)
			nl = text.size();
		std::string line = text.substr(pos, nl - pos);
		pos = nl + 1;
		long long size;
		unsigned crc;
		int n = 0;
		if (sscanf(line.c_str(), "%lld %x %n", &size, &crc, &n) >= 2 && n > 0)
		{
			std::string path = line.substr(n);
			while (!path.empty() && (path.back() == '\r' || path.back() == ' '))
				path.pop_back();
			out[path] = { size, crc };
		}
	}
	return out;
}

static void writeManifest(const std::map<std::string, Entry>& m)
{
	std::string tmp = std::string(kManifest) + ".tmp";
	FILE* f = fopen(tmp.c_str(), "w");
	if (!f)
		return;
	for (auto& e : m)
		fprintf(f, "%lld %08x %s\n", e.second.size, e.second.crc, e.first.c_str());
	fclose(f);
	remove(kManifest);
	rename(tmp.c_str(), kManifest);
}

static bool readFile(const char* path, std::string& out)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	char buf[8192];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out.append(buf, n);
	fclose(f);
	return true;
}

// CRC32 of a file on the SD card, or false when it can't be read
static bool fileCrc(const std::string& path, unsigned* crc)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	std::vector<unsigned char> buf(64 * 1024);
	uLong c = crc32(0L, Z_NULL, 0);
	size_t n;
	while ((n = fread(buf.data(), 1, buf.size(), f)) > 0)
		c = crc32(c, buf.data(), n);
	fclose(f);
	*crc = (unsigned)c;
	return true;
}

// ---- SD writer: a thread writes verified files while the next ones download (SD card writes,
// file creation above all in the big sound folder, are slow; waiting for them would stall the
// connection). Each file is written straight to its name, one directory operation instead of
// create + delete + rename; only written files enter the manifest.
struct WriteJob { std::string name, path; std::vector<char> data; Entry entry; };
static LightLock s_wLock;
static std::deque<WriteJob> s_wQueue;
static std::vector<std::pair<std::string, Entry>> s_wDone;
static long long s_wQueuedBytes = 0;
static bool s_wQuit = false;
static Thread s_writer = nullptr;
static u64 s_wBusyMs = 0;                    // time spent writing (logged per pack)
static int s_wFailed = 0;

static void writerMain(void*)
{
	for (;;)
	{
		LightLock_Lock(&s_wLock);
		if (s_wQueue.empty())
		{
			bool quit = s_wQuit;
			LightLock_Unlock(&s_wLock);
			if (quit)
				return;
			svcSleepThread(2000000LL);
			continue;
		}
		WriteJob job = std::move(s_wQueue.front());
		s_wQueue.pop_front();
		LightLock_Unlock(&s_wLock);
		u64 t0 = osGetTime();
		mkdirs(job.path);
		FILE* f = fopen(job.path.c_str(), "wb");
		bool ok = f && (job.data.empty() || fwrite(job.data.data(), 1, job.data.size(), f) == job.data.size());
		if (f)
			fclose(f);
		LightLock_Lock(&s_wLock);
		s_wBusyMs += osGetTime() - t0;
		s_wQueuedBytes -= job.data.size();
		if (ok)
			s_wDone.push_back({ job.name, job.entry });
		else
			s_wFailed++;
		LightLock_Unlock(&s_wLock);
	}
}

static void writerStart()
{
	LightLock_Init(&s_wLock);
	s_wQuit = false;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	s_writer = threadCreate(writerMain, nullptr, 32 * 1024, prio + 1, -2, false);
}

// Queues a verified file; waits while too much is waiting to be written
static void writerQueue(WriteJob&& job)
{
	for (;;)
	{
		LightLock_Lock(&s_wLock);
		bool room = s_wQueuedBytes < (8 << 20) || s_wQueue.empty();
		if (room)
		{
			s_wQueuedBytes += job.data.size();
			s_wQueue.push_back(std::move(job));
		}
		LightLock_Unlock(&s_wLock);
		if (room)
			return;
		svcSleepThread(5000000LL);
		userStops();
	}
}

// Files written since the last call go into the manifest
static void writerCollect(std::map<std::string, Entry>& local)
{
	LightLock_Lock(&s_wLock);
	for (auto& d : s_wDone)
		local[d.first] = d.second;
	s_wDone.clear();
	LightLock_Unlock(&s_wLock);
}

// Waits for the queue to empty, showing how much is left (B / HOME drop what's still queued:
// those files are fetched again next time), then ends the thread
static void writerStop(std::map<std::string, Entry>& local, const Progress& progress)
{
	if (!s_writer)
		return;
	u64 lastDraw = 0;
	for (;;)
	{
		LightLock_Lock(&s_wLock);
		size_t left = s_wQueue.size();
		long long bytes = s_wQueuedBytes;
		if (userStops() && left)
		{
			for (auto& j : s_wQueue)
				s_wQueuedBytes -= j.data.size();
			s_wQueue.clear();
			logf("dev: %d queued files dropped (fetched next time)", (int)left);
			left = 0;
		}
		LightLock_Unlock(&s_wLock);
		if (!left)
			break;
		if (osGetTime() - lastDraw > 250)
		{
			lastDraw = osGetTime();
			progress("Finishing writing to the SD card...", std::to_string(left) + " files (" + sizeText(bytes)
				+ ") left\n(hold B to stop: they download again next time)");
		}
		svcSleepThread(20000000LL);
	}
	LightLock_Lock(&s_wLock);
	s_wQuit = true;
	LightLock_Unlock(&s_wLock);
	threadJoin(s_writer, U64_MAX);
	threadFree(s_writer);
	s_writer = nullptr;
	writerCollect(local);
}

// One pack of files: each is checked against its CRC and handed to the writer. Returns false
// when the connection dropped (what arrived complete is kept).
static bool fetchPack(const std::vector<std::string>& names, const std::map<std::string, Entry>& remote,
	std::map<std::string, Entry>& local, long long& done, const std::function<void(const std::string&)>& onFile,
	const std::function<void()>& tick, int& bad, size_t& processed)
{
	processed = 0;
	std::string list;
	for (auto& n : names)
		list += n + "\n";
	Body body;
	int status = request("POST", "/dev/pack", "Content-Type: text/plain\r\n", list, body);
	if (status != 200)
	{
		finish(body, false);
		if (status > 0)
			logf("dev: pack: HTTP %d", status);
		return false;
	}
	for (;;)
	{
		// "size crc path\n"
		std::string line;
		char c;
		int r;
		while ((r = body.read(&c, 1)) == 1 && c != '\n')
			line += c;
		if (r == 0 && line.empty())
		{
			finish(body, true);
			return true;
		}
		if (r != 1)
		{
			finish(body, false);
			return false;
		}
		long long size;
		unsigned crc;
		int n = 0;
		if (sscanf(line.c_str(), "%lld %x %n", &size, &crc, &n) < 2 || n <= 0)
		{
			logf("dev: pack: bad header %.60s", line.c_str());
			finish(body, false);
			return false;
		}
		std::string name = line.substr(n);
		std::string path = std::string(kDataDir) + "/" + name;
		// The whole file in memory, then one write: small SD writes are very slow on the 3DS
		std::vector<char> data(size);
		long long have = 0;
		while (have < size)
		{
			int k = body.read(data.data() + have, (int)std::min<long long>(64 * 1024, size - have));
			if (k <= 0 || userStops())
			{
				finish(body, false);
				return false;
			}
			have += k;
			done += k;
			tick();
		}
		uLong got = crc32(crc32(0L, Z_NULL, 0), (const Bytef*)data.data(), size);
		auto it = remote.find(name);
		if ((unsigned)got != crc || it == remote.end() || it->second.crc != crc)
		{
			// Damaged on the way, or changed on the PC meanwhile: fetched again next time
			bad++;
			processed++;
			logf("dev: %s arrived damaged (crc %08lx, want %08x)", name.c_str(), (unsigned long)got, crc);
			continue;
		}
		writerQueue({ name, path, std::move(data), { size, crc } });
		processed++;
		onFile(name);
	}
}

static bool syncData(const Progress& progress)
{
	std::string text;
	if (!getText("/dev/manifest", text))
	{
		logf("dev: no manifest");
		return false;
	}
	std::map<std::string, Entry> remote = parseManifest(text), local;
	std::string localText;
	if (readFile(kManifest, localText))
		local = parseManifest(localText);

	// Folders the PC no longer has at all (an older layout: sound/ and textures/ before they were
	// split into snd/ and tex/ subfolders) go in one recursive delete each: deleting their
	// thousands of files one by one would scan the folder for every file
	{
		std::set<std::string> tops;
		for (auto& e : remote)
		{
			size_t slash = e.first.find('/');
			if (slash != std::string::npos)
				tops.insert(e.first.substr(0, slash));
		}
		std::vector<std::string> gone;
		if (DIR* dir = opendir(kDataDir))
		{
			while (struct dirent* de = readdir(dir))
				if (de->d_type == DT_DIR && de->d_name[0] != '.' && !tops.count(de->d_name))
					gone.push_back(de->d_name);
			closedir(dir);
		}
		if (!gone.empty())
		{
			FS_Archive sdmc;
			if (R_SUCCEEDED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
			{
				for (auto& g : gone)
				{
					// Renamed into the trash (one quick operation); devEmptyTrash deletes it bit by bit
					// in the background (a recursive delete of thousands of files takes many minutes)
					progress("Moving old game data aside...", g);
					std::string from = "/3ds/mw3ds/data/" + g;
					std::string to = std::string(kTrash) + "/" + g + "-" + std::to_string(osGetTime());
					FSUSER_CreateDirectory(sdmc, fsMakePath(PATH_ASCII, kTrash), 0);
					Result r = FSUSER_RenameDirectory(sdmc, fsMakePath(PATH_ASCII, from.c_str()), sdmc,
						fsMakePath(PATH_ASCII, to.c_str()));
					logf("dev: moved folder %s to the trash (0x%08lx)", g.c_str(), (unsigned long)r);
					for (auto it = local.begin(); it != local.end();)
						it = it->first.compare(0, g.size() + 1, g + "/") == 0 ? local.erase(it) : std::next(it);
				}
				FSUSER_CloseArchive(sdmc);
				writeManifest(local);
			}
		}
	}

	// Files on the card without a record (an older copy, a lost manifest): adopted when their
	// size and CRC already match, instead of downloaded again
	// Which of them exist at all comes from one listing per folder: asking the SD card about each
	// file scans its whole folder every time (thousands of files in sound/)
	std::vector<std::string> unknown;
	std::set<std::string> dirs;
	for (auto& e : remote)
		if (!local.count(e.first))
		{
			unknown.push_back(e.first);
			size_t slash = e.first.rfind('/');
			dirs.insert(slash == std::string::npos ? "" : e.first.substr(0, slash));
		}
	std::set<std::string> present;
	for (auto& d : dirs)
	{
		std::string full = std::string(kDataDir) + (d.empty() ? "" : "/" + d);
		DIR* dir = opendir(full.c_str());
		if (!dir)
			continue;
		while (struct dirent* de = readdir(dir))
			present.insert(d.empty() ? std::string(de->d_name) : d + "/" + de->d_name);
		closedir(dir);
	}
	int adopted = 0, checked = 0;
	u64 lastDraw = 0;
	for (size_t i = 0; i < unknown.size() && !userStops(); i++)
	{
		const std::string& name = unknown[i];
		if (!present.count(name))
			continue;
		const Entry& e = remote[name];
		std::string path = std::string(kDataDir) + "/" + name;
		struct stat st;
		unsigned crc;
		checked++;
		if (stat(path.c_str(), &st) == 0 && st.st_size == e.size && fileCrc(path, &crc) && crc == e.crc)
		{
			local[name] = e;
			adopted++;
		}
		if (osGetTime() - lastDraw > 250)
		{
			lastDraw = osGetTime();
			progress("Checking game data on the SD card...", std::to_string(checked) + " files checked\n" + name
				+ "\n(hold B to stop and play)");
		}
	}
	if (s_stop || s_quit)
	{
		writeManifest(local);
		logf("dev: data check stopped (%d adopted)", adopted);
		return false;
	}

	std::vector<std::string> need;
	long long needBytes = 0;
	for (auto& e : remote)
	{
		auto it = local.find(e.first);
		if (it == local.end() || it->second.size != e.second.size || it->second.crc != e.second.crc)
		{
			need.push_back(e.first);
			needBytes += e.second.size;
		}
	}
	// Files the PC no longer has
	int removed = 0;
	for (auto it = local.begin(); it != local.end();)
		if (!remote.count(it->first))
		{
			remove((std::string(kDataDir) + "/" + it->first).c_str());
			it = local.erase(it);
			removed++;
		}
		else
			++it;
	logf("dev: data: %d of %d files to fetch (%s), %d already on the card, %d removed", (int)need.size(),
		(int)remote.size(), sizeText(needBytes).c_str(), adopted, removed);
	if (adopted || removed)
		writeManifest(local);
	if (need.empty())
		return true;

	mkdir(kDataDir, 0777);
	writerStart();
	long long done = 0;
	int files = 0, bad = 0, drops = 0;
	u64 start = osGetTime();
	size_t next = 0;
	std::string current;
	auto draw = [&](bool force) {
		u64 t = osGetTime();
		if (!force && t - lastDraw < 250)
			return;
		lastDraw = t;
		float secs = (t - start) / 1000.0f;
		float rate = secs > 0.5f ? done / 1024.0f / secs : 0.0f;
		char line[200];
		int eta = rate > 1.0f ? (int)((needBytes - done) / 1024.0f / rate) : -1;
		snprintf(line, sizeof(line), "%d / %d files, %s of %s\n%.0f KB/s%s", files, (int)need.size(),
			sizeText(done).c_str(), sizeText(needBytes).c_str(), rate,
			eta >= 0 ? (", about " + std::to_string(eta / 60) + " min " + std::to_string(eta % 60) + " s left").c_str() : "");
		progress("Updating game data...", std::string(line) + "\n" + current + "\n(hold B to stop and play)");
	};
	while (next < need.size())
	{
		if (userStops())
		{
			logf(s_quit ? "dev: closed from the HOME menu during the data update" : "dev: data update stopped with B");
			break;
		}
		// The next pack: up to kPackFiles files / kPackBytes
		std::vector<std::string> pack;
		long long bytes = 0;
		for (size_t i = next; i < need.size() && pack.size() < kPackFiles && (pack.empty() || bytes < kPackBytes); i++)
		{
			pack.push_back(need[i]);
			bytes += remote[need[i]].size;
		}
		s_recvCalls = 0;
		s_recvBytes = 0;
		s_maxWaitMs = 0;
		u64 packStart = osGetTime();
		size_t processed = 0;
		bool ok = fetchPack(pack, remote, local, done, [&](const std::string& name) {
			files++;
			current = name;
			draw(false);
		}, [&]() { draw(false); }, bad, processed);
		{
			float secs = (osGetTime() - packStart) / 1000.0f;
			LightLock_Lock(&s_wLock);
			u64 busy = s_wBusyMs;
			s_wBusyMs = 0;
			LightLock_Unlock(&s_wLock);
			logf("dev: pack %s: %s in %.1f s (%.0f KB/s), %lu receives, longest wait %llu ms, SD writing %llu ms",
				ok ? "done" : "cut", sizeText(s_recvBytes).c_str(), secs, s_recvBytes / 1024.0f / fmaxf(secs, 0.001f),
				(unsigned long)s_recvCalls, s_maxWaitMs, busy);
		}
		if (s_stop || s_quit)
		{
			logf(s_quit ? "dev: closed from the HOME menu during the data update" : "dev: data update stopped with B");
			break;
		}
		writerCollect(local);
		// the record of what's on the card: every 20 s (rewriting thousands of lines per pack was slow)
		static u64 lastManifest = 0;
		if (osGetTime() - lastManifest > 20000)
		{
			writeManifest(local);
			lastManifest = osGetTime();
		}
		if (ok)
			next += pack.size();
		else
		{
			// Carry on from the first file that didn't arrive (files come in the order asked)
			drops++;
			next += processed;
			logf("dev: pack dropped after %d files (%d drops)", (int)processed, drops);
			if (drops > 20)
				break;
			svcSleepThread(1000000000LL);
		}
		draw(true);
	}
	// Everything verified gets written, even after B
	writerStop(local, progress);
	writeManifest(local);
	if (s_wFailed)
		logf("dev: %d files could not be written", s_wFailed);
	logf("dev: data: %d files, %s in %llu s, %d damaged, %d drops", files, sizeText(done).c_str(),
		(osGetTime() - start) / 1000, bad, drops);
	return next >= need.size() && bad == 0;
}

// ---- Code

static std::string trim(std::string s)
{
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
		s.pop_back();
	return s;
}

static bool installCia(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	if (R_FAILED(amInit()))
	{
		fclose(f);
		return false;
	}
	Handle cia;
	Result r = AM_StartCiaInstall(MEDIATYPE_SD, &cia);
	bool ok = R_SUCCEEDED(r);
	if (ok)
	{
		std::vector<u8> buf(256 * 1024);
		u64 offset = 0;
		size_t n;
		while (ok && (n = fread(buf.data(), 1, buf.size(), f)) > 0)
		{
			u32 written = 0;
			ok = R_SUCCEEDED(FSFILE_Write(cia, &written, offset, buf.data(), n, 0)) && written == n;
			offset += n;
		}
		if (ok)
		{
			r = AM_FinishCiaInstall(cia);
			ok = R_SUCCEEDED(r);
		}
		else
			AM_CancelCIAInstall(cia);
	}
	logf("dev: install %s (0x%08lx)", ok ? "done" : "FAILED", (unsigned long)r);
	fclose(f);
	amExit();
	return ok;
}

DevUpdateResult devUpdate(const Progress& progress)
{
	// The dev CIA carries devhost.txt; sdmc:/3ds/mw3ds/devhost.txt tests the data sync elsewhere
	// (the emulator, a .3dsx), where there is no dev title to update
	FILE* f = fopen("romfs:/devhost.txt", "r");
	if (!f)
		f = fopen("sdmc:/3ds/mw3ds/devhost.txt", "r");
	if (!f)
		return DEV_NONE;
	char host[64] = {};
	int n = fscanf(f, "%63s %d", host, &s_port);
	fclose(f);
	if (n != 2)
		return DEV_NONE;
	s_host = host;
	hidScanInput();
	if (hidKeysHeld() & KEY_B)
	{
		logf("dev: update skipped (B held)");
		return DEV_NONE;
	}
	if (!logNetEnsure())
		return DEV_NONE;
	std::string mine;
	bool devTitle = readFile("romfs:/buildid.txt", mine);
	mine = trim(mine);
	progress("Checking for updates...", "PC " + s_host + ":" + std::to_string(s_port) + "\nthis build " + mine
		+ "\n(hold B at launch to skip)");
	// One quick try: a PC that's off shouldn't hold up the game
	s_timeoutMs = kProbeTimeoutMs;
	std::string theirs;
	bool reached = getText("/dev/version", theirs, 1);
	s_timeoutMs = kTimeoutMs;
	if (!reached)
	{
		logf("dev: PC %s:%d not reachable, playing this build", host, s_port);
		dropConnection();
		return DEV_NONE;
	}
	theirs = trim(theirs);
	logf("dev: this build %s, PC has %s", mine.c_str(), theirs.c_str());
	// New code first, so a fix arrives even while data is still syncing (the new build syncs next)
	if (theirs == mine || theirs.empty() || !devTitle)
	{
		bool dataOk = syncData(progress);
		dropConnection();
		if (s_quit)
			return DEV_QUIT;
		return dataOk ? DEV_UPDATED : DEV_NONE;
	}
	u64 start = osGetTime();
	bool got = download("/dev/code.cia", kCodeCia, [&](long long now, long long total) {
		progress("Downloading the new build...", sizeText(now) + (total > 0 ? " of " + sizeText(total) : ""));
	});
	dropConnection();
	if (!got)
	{
		logf("dev: code download failed, playing this build");
		return DEV_NONE;
	}
	logf("dev: code downloaded in %llu ms", osGetTime() - start);
	progress("Installing the new build...", theirs);
	bool ok = installCia(kCodeCia);
	remove(kCodeCia);
	if (!ok)
		return DEV_NONE;
	// Start the freshly installed title over this one
	u64 programId = 0;
	APT_GetProgramID(&programId);
	logf("dev: relaunching %016llx", programId);
	progress("Restarting into the new build...", theirs);
	u8 param[0x300] = {}, hmac[0x20] = {};
	APT_PrepareToDoApplicationJump(0, programId, MEDIATYPE_SD);
	APT_DoApplicationJump(param, sizeof(param), hmac);
	return DEV_RELAUNCH;
}

// ---- Background trash deletion: one file at a time with pauses, so it never holds the SD card for
// long while the game loads

static void deleteTree(const std::string& dir)
{
	DIR* d = opendir(dir.c_str());
	if (!d)
		return;
	std::vector<std::pair<std::string, bool>> entries;
	while (struct dirent* de = readdir(d))
		if (de->d_name[0] != '.')
			entries.push_back({ de->d_name, de->d_type == DT_DIR });
	closedir(d);
	for (auto& e : entries)
	{
		std::string path = dir + "/" + e.first;
		if (e.second)
			deleteTree(path);
		else
			remove(path.c_str());
		svcSleepThread(3000000LL);
	}
	rmdir(dir.c_str());
}

static void trashMain(void*)
{
	u64 start = osGetTime();
	deleteTree(std::string("sdmc:") + kTrash);
	logf("dev: trash emptied in %llu s", (osGetTime() - start) / 1000);
}

void devEmptyTrash()
{
	struct stat st;
	if (stat((std::string("sdmc:") + kTrash).c_str(), &st) != 0)
		return;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	logf("dev: emptying the trash in the background");
	threadCreate(trashMain, nullptr, 64 * 1024, prio + 2 > 0x3F ? 0x3F : prio + 2, -2, true);
}
