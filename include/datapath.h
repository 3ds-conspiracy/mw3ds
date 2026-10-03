#pragma once

#include <string>
#include <zlib.h>

// Sounds and textures live in 64 subfolders chosen by a checksum of the file name (the SD card's
// file system scans a whole folder to open or create a file; thousands in one folder crawl).
// tools/convert/textures.py shard_dir computes the same.
inline std::string shardedPath(const std::string& dataDir, const char* folder, const std::string& name);

// A cell's file (stem: its name without extension; ext: ".cel", ".act", ".json"): cells/<xx>/<stem><ext>
// (tools/convert/level.py shard_cells), else the older flat cells/<stem><ext>
inline std::string cellPath(const std::string& dataDir, const std::string& stem, const char* ext);

inline std::string shardedPath(const std::string& dataDir, const char* folder, const std::string& name)
{
	char shard[4];
	unsigned c = crc32(0L, (const Bytef*)name.data(), name.size()) & 63;
	snprintf(shard, sizeof(shard), "%02x", c);
	return dataDir + "/" + folder + "/" + shard + "/" + name;
}

inline std::string cellPath(const std::string& dataDir, const std::string& stem, const char* ext)
{
	std::string p = shardedPath(dataDir, "cells", stem) + ext;
	if (FILE* f = fopen(p.c_str(), "rb"))
	{
		fclose(f);
		return p;
	}
	return dataDir + "/cells/" + stem + ext;
}
