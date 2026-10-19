#include "components/compiler/compiler.hpp"

#include <cstdlib>
#include <cstring>
#include <deque>

#include "extern/cjson/cJSON.h"
#include "components/compiler/extensions0.hpp"

// A port of tools/convert/mwscript.py, rule for rule: see there for why each odd case is read the way it is
// (OpenMW's scanner and parsers, and what Morrowind's own compiler let through). Keep the two in step:
// tools/test/script_compile_check.py compares them on every script of the game.

namespace
{

template <typename T, size_t N>
const T* findSorted(const T (&table)[N], const std::string& name)
{
	size_t lo = 0, hi = N;
	while (lo < hi)
	{
		size_t mid = (lo + hi) / 2;
		int c = strcmp(table[mid].name, name.c_str());
		if (c == 0)
			return &table[mid];
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return nullptr;
}

template <size_t N>
bool inList(const char* (&list)[N], const std::string& s)
{
	for (const char* x : list)
		if (s == x)
			return true;
	return false;
}

bool startsWith(const std::string& s, const char* p)
{
	return s.compare(0, strlen(p), p) == 0;
}

bool endsWith(const std::string& s, const char* p)
{
	size_t n = strlen(p);
	return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}

// (result type, signature) of a function, false when it is none: the table, then the stat families
bool funcInfo(const std::string& name, char* type, std::string* sig)
{
	if (const ScFunc* f = findSorted(kScFuncs, name))
	{
		*type = f->type;
		*sig = f->sig;
		return true;
	}
	if (startsWith(name, "get"))
	{
		std::string stat = name.substr(3);
		*type = 'f';
		if (inList(kScAttrs, stat) || inList(kScSkills, stat))
		{
			*sig = "";
			return true;
		}
		if (inList(kScDynamics, stat))
		{
			*sig = "x";
			return true;
		}
		if (endsWith(stat, "getratio") && inList(kScDynamics, stat.substr(0, stat.size() - 8)))
		{
			*sig = "";
			return true;
		}
	}
	return false;
}

bool instrSignature(const std::string& name, std::string* sig)
{
	if (const ScInstr* in = findSorted(kScInstrs, name))
	{
		*sig = in->sig;
		return true;
	}
	for (const char* prefix : { "modcurrent", "set", "mod" })
		if (startsWith(name, prefix) && name != prefix)
		{
			std::string stat = name.substr(strlen(prefix));
			if (inList(kScAttrs, stat) || inList(kScSkills, stat) || inList(kScDynamics, stat))
			{
				*sig = "f";
				return true;
			}
		}
	return false;
}

std::string lower(std::string s)
{
	for (auto& ch : s)
		if (ch >= 'A' && ch <= 'Z')
			ch = (char)(ch - 'A' + 'a');
	return s;
}

// ---- Tokens (mwscript.py's TOKEN pattern, its alternatives tried in order)

struct Tok
{
	char kind;            // 's' string, 'i' name, 'n' number, 'o' operator
	std::string text;
	bool is(char k, const char* t) const { return kind == k && text == t; }
	bool op(const char* t) const { return is('o', t); }
};

bool pySpace(unsigned char c)
{
	return c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1c && c <= 0x1f);
}

bool digit(char c) { return c >= '0' && c <= '9'; }
bool alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool nameChar(char c) { return alpha(c) || digit(c) || c == '\'' || c == '`'; }

// name characters, then any number of '-' + name characters (Ald-ruhn)
size_t nameTail(const std::string& s, size_t p)
{
	while (p < s.size() && nameChar(s[p]))
		p++;
	while (p + 1 < s.size() && s[p] == '-' && nameChar(s[p + 1]))
	{
		p++;
		while (p < s.size() && nameChar(s[p]))
			p++;
	}
	return p;
}

std::vector<Tok> tokenize(const std::string& line)
{
	static const char* kOps[] = { "->", "==", "!=", "<>", "<=", ">=", "=<", "=>", "<", ">", "=", "+", "-", "*", "/",
		"(", ")", "[", "]", ",", "." };
	std::vector<Tok> out;
	size_t pos = 0, n = line.size();
	while (n > 0 && pySpace((unsigned char)line[n - 1]))
		n--;
	std::string s = line.substr(0, n);
	while (pos < n)
	{
		char c = s[pos];
		if (pySpace((unsigned char)c))
		{
			pos++;
			continue;
		}
		size_t end = 0;
		char kind = 0;
		if (c == '"')
		{
			end = pos + 1;
			while (end < n && s[end] != '"' && s[end] != '\n')
				end++;
			if (end < n && s[end] == '"')
				end++;
			kind = 's';
		}
		else if (digit(c))
		{
			size_t d = pos;
			while (d < n && digit(s[d]))
				d++;
			if (d < n && (alpha(s[d]) || s[d] == '\'' || s[d] == '`'))
			{
				end = nameTail(s, d + 1);          // a name that starts with digits (3rd_door)
				kind = 'i';
			}
			else
			{
				end = d;
				if (d < n && s[d] == '.')
				{
					end = d + 1;
					while (end < n && digit(s[end]))
						end++;
				}
				kind = 'n';
			}
		}
		else if (c == '.' && pos + 1 < n && digit(s[pos + 1]))
		{
			end = pos + 1;
			while (end < n && digit(s[end]))
				end++;
			kind = 'n';
		}
		if (!kind)
			for (const char* o : kOps)
			{
				size_t len = strlen(o);
				if (s.compare(pos, len, o) == 0)
				{
					end = pos + len;
					kind = 'o';
					break;
				}
			}
		if (!kind && alpha(c))
		{
			end = nameTail(s, pos + 1);
			kind = 'i';
		}
		if (!kind)
		{
			pos++;                         // a stray character: skipped
			continue;
		}
		std::string text = s.substr(pos, end - pos);
		if (kind == 's')
		{
			size_t a = 0, b = text.size();
			while (a < b && text[a] == '"')
				a++;
			while (b > a && text[b - 1] == '"')
				b--;
			text = text.substr(a, b - a);
		}
		else if (kind == 'o' && (text == "[" || text == "]"))
			text = text == "[" ? "(" : ")";          // OpenMW reads brackets as parentheses
		out.push_back({ kind, text });
		pos = end;
	}
	return out;
}

// ---- The tree: mwscript.py's lists, numbers, strings and None

struct V
{
	enum Kind : unsigned char { INT, FLT, STR, NUL, LIST } k = NUL;
	double n = 0.0;
	std::string s;
	std::vector<V*> l;
};

struct Frame
{
	char kind;            // 'r' root, 'w' while, 'd' dead code (a second else), 'i' if
	V* node;              // the body list, or the if node
	bool inElse;
};

struct Compiler
{
	const std::unordered_map<std::string, char>* globals;
	std::vector<std::pair<std::string, std::string>> locals;
	std::string pendingRef;
	bool hasPendingRef = false;
	bool err = false;
	std::string errMsg;
	std::deque<V> pool;

	V* mk(V::Kind k)
	{
		pool.emplace_back();
		pool.back().k = k;
		return &pool.back();
	}
	V* num(double n, bool whole) { V* v = mk(whole ? V::INT : V::FLT); v->n = n; return v; }
	V* str(const std::string& s) { V* v = mk(V::STR); v->s = s; return v; }
	V* nul() { return mk(V::NUL); }
	V* list(std::initializer_list<V*> items) { V* v = mk(V::LIST); v->l = items; return v; }
	V* fail(const std::string& msg)
	{
		if (!err)
		{
			err = true;
			errMsg = msg;
		}
		return nul();
	}
	V* numberTok(const std::string& text, bool neg)
	{
		bool whole = true;
		for (char ch : text)
			if (!digit(ch))
				whole = false;
		double v = atof(text.c_str());
		return num(neg ? -v : v, whole);
	}

	bool isLocal(const std::string& name) const
	{
		for (auto& l : locals)
			if (l.second == name)
				return true;
		return false;
	}
	bool isGlobal(const std::string& name) const { return globals && globals->count(name); }

	// ---- types: whole numbers divide as whole numbers
	char localType(const std::string& name) const
	{
		for (auto& l : locals)
			if (l.second == name)
				return l.first == "short" || l.first == "long" ? 'l' : 'f';
		return 'f';
	}
	char globalType(const std::string& name) const
	{
		if (!globals)
			return 'f';
		auto it = globals->find(name);
		return it != globals->end() && (it->second == 's' || it->second == 'l') ? 'l' : 'f';
	}
	char ntype(const V* a) const
	{
		if (a->k == V::INT)
			return 'l';
		if (a->k != V::LIST || a->l.empty() || a->l[0]->k != V::STR)
			return 'f';
		const std::string& tag = a->l[0]->s;
		if (tag == "l")
			return localType(a->l[1]->s);
		if (tag == "g")
			return globalType(a->l[1]->s);
		if (tag == "c")
		{
			char t;
			std::string sig;
			return funcInfo(a->l[2]->s, &t, &sig) ? t : 'f';
		}
		if (tag == "neg")
			return ntype(a->l[1]);
		if (tag == "==" || tag == "!=" || tag == "<" || tag == "<=" || tag == ">" || tag == ">=")
			return 'l';
		if (tag == "+" || tag == "-" || tag == "*" || tag == "/" || tag == "//")
			return ntype(a->l[1]) == 'l' && ntype(a->l[2]) == 'l' ? 'l' : 'f';
		return 'f';
	}

	// ---- expressions
	V* expr(const std::vector<Tok>& t, size_t& i) { return comparison(t, i); }

	V* comparison(const std::vector<Tok>& t, size_t& i)
	{
		V* a = additive(t, i);
		while (!err && i < t.size() && t[i].kind == 'o')
		{
			// OpenMW's scanner reads =< and => as ==, and <> as <
			const std::string& o = t[i].text;
			const char* op = o == "==" || o == "=" || o == "=<" || o == "=>" ? "==" : o == "<>" ? "<"
				: o == "!=" ? "!=" : o == "<" ? "<" : o == "<=" ? "<=" : o == ">" ? ">" : o == ">=" ? ">=" : nullptr;
			if (!op)
				break;
			i++;
			V* b = additive(t, i);
			a = list({ str(op), a, b });
		}
		return a;
	}

	V* additive(const std::vector<Tok>& t, size_t& i)
	{
		V* a = term(t, i);
		while (!err && i < t.size() && (t[i].op("+") || t[i].op("-")))
		{
			std::string op = t[i].text;
			i++;
			V* b = term(t, i);
			a = list({ str(op), a, b });
		}
		return a;
	}

	V* term(const std::vector<Tok>& t, size_t& i)
	{
		V* a = unary(t, i);
		while (!err && i < t.size() && (t[i].op("*") || t[i].op("/")))
		{
			std::string op = t[i].text;
			i++;
			V* b = unary(t, i);
			if (op == "/" && ntype(a) == 'l' && ntype(b) == 'l')
				op = "//";
			a = list({ str(op), a, b });
		}
		return a;
	}

	V* unary(const std::vector<Tok>& t, size_t& i)
	{
		if (i < t.size() && t[i].op("-"))
		{
			i++;
			V* a = unary(t, i);
			if (a->k == V::INT || a->k == V::FLT)
			{
				a->n = -a->n;
				return a;
			}
			return list({ str("neg"), a });
		}
		if (i < t.size() && t[i].op("+"))
		{
			i++;
			return unary(t, i);                // a unary plus is dropped
		}
		return primary(t, i);
	}

	void skipCommas(const std::vector<Tok>& t, size_t& i)
	{
		while (i < t.size() && t[i].op(","))
			i++;
	}

	V* primary(const std::vector<Tok>& t, size_t& i)
	{
		skipCommas(t, i);
		if (i >= t.size())
			return fail("expression expected");
		const Tok& k = t[i];
		if (k.kind == 'n')
		{
			i++;
			return numberTok(k.text, false);
		}
		if (k.op("("))
		{
			i++;
			V* a = expr(t, i);
			skipCommas(t, i);
			if (i < t.size() && t[i].op(")"))
				i++;
			return a;
		}
		if (k.kind == 's' || k.kind == 'i')
		{
			// explicit reference: X->Func args
			if (i + 2 < t.size() && t[i + 1].op("->"))
			{
				// X->( OnDeath == 1 ): the reference holds for the first function inside the parenthesis
				if (t[i + 2].op("("))
				{
					pendingRef = lower(k.text);
					hasPendingRef = true;
					i += 3;
					V* a = expr(t, i);
					hasPendingRef = false;
					skipCommas(t, i);
					if (i < t.size() && t[i].op(")"))
						i++;
					return a;
				}
				i += 2;
				return call(t, i, str(lower(k.text)));
			}
			// remote variable: X.var
			if (i + 2 < t.size() && t[i + 1].op(".") && t[i + 2].kind == 'i')
			{
				V* r = list({ str("r"), str(lower(k.text)), str(lower(t[i + 2].text)) });
				i += 3;
				return r;
			}
			if (k.kind == 's')
			{
				i++;
				return list({ str("s"), str(k.text) });
			}
			std::string name = lower(k.text);
			if (isLocal(name))
			{
				i++;
				return list({ str("l"), str(name) });
			}
			if (isGlobal(name))
			{
				i++;
				return list({ str("g"), str(name) });
			}
			V* ref = hasPendingRef ? str(pendingRef) : nul();
			hasPendingRef = false;
			return call(t, i, ref);
		}
		return fail("unexpected '" + k.text + "'");
	}

	V* call(const std::vector<Tok>& t, size_t& i, V* ref)
	{
		std::string name = lower(t[i].text);
		i++;
		char type;
		std::string sig;
		V* args = mk(V::LIST);
		if (funcInfo(name, &type, &sig))
			readArgs(t, i, sig, true, args);
		else
		{
			const ScArity* ar = findSorted(kScExprArity, name);
			for (int k = 0; k < (ar ? ar->count : 0) && !err; k++)
			{
				skipCommas(t, i);
				if (i >= t.size() || t[i].op(")"))
					break;
				args->l.push_back(arg(t, i));
			}
		}
		return list({ str("c"), ref, str(name), args });
	}

	bool operandStart(const Tok& k) const
	{
		return k.kind != 'o' || k.text == "(" || k.text == "-" || k.text == "+";
	}

	// The arguments as OpenMW's signature says (mwscript.py's args)
	void readArgs(const std::vector<Tok>& t, size_t& i, const std::string& sig, bool inExpr, V* out)
	{
		for (char ch : sig)
		{
			if (err)
				return;
			if (ch == '/' || ch == 'j')
				continue;
			skipCommas(t, i);
			bool stringArg = strchr("Scxz", ch) != nullptr;
			if (i >= t.size() || !operandStart(t[i]) || (stringArg && t[i].kind == 'o'))
				break;
			V* a;
			if (strchr("lfX", ch))
				a = term(t, i);
			else
			{
				a = arg(t, i);
				// an id of several words without quotes (HasSoulGem dagoth drals): the words that follow
				while (inExpr && a->k == V::LIST && a->l[0]->s == "s" && i < t.size() && t[i].kind == 'i'
					&& !isLocal(lower(t[i].text)) && !isGlobal(lower(t[i].text)))
				{
					a = list({ str("s"), str(a->l[1]->s + " " + t[i].text) });
					i++;
				}
			}
			out->l.push_back(a);
		}
	}

	V* arg(const std::vector<Tok>& t, size_t& i)
	{
		const Tok& k = t[i];
		if (k.op("-") && i + 1 < t.size() && t[i + 1].kind == 'n')
		{
			i += 2;
			return numberTok(t[i - 1].text, true);
		}
		if (k.kind == 'n')
		{
			i++;
			return numberTok(k.text, false);
		}
		if (k.kind == 's')
		{
			if (i + 2 < t.size() && t[i + 1].op(".") && t[i + 2].kind == 'i')
			{
				V* r = list({ str("r"), str(lower(k.text)), str(lower(t[i + 2].text)) });
				i += 3;
				return r;
			}
			i++;
			return list({ str("s"), str(k.text) });
		}
		if (k.kind == 'i')
		{
			std::string name = lower(k.text);
			i++;
			if (isLocal(name))
				return list({ str("l"), str(name) });
			if (isGlobal(name))
				return list({ str("g"), str(name) });
			return list({ str("s"), str(k.text) });
		}
		i++;
		return list({ str("s"), str(k.text) });
	}

	// ---- statements

	// MessageBox "text %g %s" args..., then the button labels: one argument per placeholder
	V* messageBox(const std::vector<Tok>& t, size_t i)
	{
		V* args = mk(V::LIST);
		skipCommas(t, i);
		if (i >= t.size())
			return args;
		V* a = arg(t, i);
		args->l.push_back(a);
		std::string fmt = a->k == V::LIST && a->l[0]->s == "s" ? a->l[1]->s : "";
		for (size_t p = 0; p < fmt.size() && !err;)
		{
			if (fmt[p] != '%')
			{
				p++;
				continue;
			}
			if (p + 1 < fmt.size() && fmt[p + 1] == '%')
			{
				p += 2;
				continue;
			}
			size_t j = p + 1;
			while (j < fmt.size() && (fmt[j] == '.' || digit(fmt[j])))
				j++;
			if (j >= fmt.size() || !strchr("gfdis", fmt[j]))
			{
				p++;
				continue;
			}
			char conv = fmt[j];
			p = j + 1;
			skipCommas(t, i);
			if (i >= t.size() || !operandStart(t[i]))
				break;
			args->l.push_back(conv == 's' ? arg(t, i) : term(t, i));
		}
		while (i < t.size() && !err)
		{
			if (t[i].op(","))
			{
				i++;
				continue;
			}
			args->l.push_back(arg(t, i));
		}
		return args;
	}

	V* statement(const std::vector<Tok>& t)
	{
		const Tok& first = t[0];
		std::string word = first.kind == 'i' ? lower(first.text) : "";
		if (word == "set" && t.size() >= 4)
		{
			size_t to = 0;
			for (size_t k = 2; k < t.size() && !to; k++)
				if (t[k].kind == 'i' && lower(t[k].text) == "to")
					to = k;
			if (!to)
				return fail("set without 'to'");
			std::vector<const Tok*> target;
			for (size_t k = 1; k < to; k++)
				if (!t[k].op(","))
					target.push_back(&t[k]);
			V* tv;
			if (target.size() == 3 && (target[1]->op(".") || target[1]->op("->")))
				tv = list({ str("r"), str(lower(target[0]->text)), str(lower(target[2]->text)) });
			else if (target.empty())
				return fail("set: no variable");
			else
			{
				std::string name = lower(target[0]->text);
				tv = list({ str(isLocal(name) ? "l" : "g"), str(name) });
			}
			size_t i = to + 1;
			V* value = expr(t, i);
			return list({ str("set"), tv, value });
		}
		// call statement: [ref ->] func args...
		size_t i = 0;
		V* ref = nul();
		if (t.size() > 2 && t[1].op("->"))
		{
			ref = str(lower(first.text));
			i = 2;
		}
		std::string func = lower(t[i].text);
		i++;
		skipCommas(t, i);
		if (func == "messagebox")
			return list({ str("c"), ref, str(func), messageBox(t, i) });
		std::string sig;
		char type;
		bool known = instrSignature(func, &sig) || funcInfo(func, &type, &sig);
		V* args = mk(V::LIST);
		if (known)
			readArgs(t, i, sig, false, args);         // what follows the arguments is ignored
		else
			while (i < t.size() && !err)
			{
				if (t[i].op(","))
				{
					i++;
					continue;
				}
				args->l.push_back(arg(t, i));
			}
		return list({ str("c"), ref, str(func), args });
	}

	static std::vector<std::vector<Tok>> splitLines(const std::string& text, bool dropLeadingOps)
	{
		std::vector<std::vector<Tok>> lines;
		size_t start = 0;
		while (start <= text.size())
		{
			size_t nl = text.find('\n', start);
			if (nl == std::string::npos)
				nl = text.size();
			std::string code;
			bool quoted = false;
			for (size_t k = start; k < nl; k++)
			{
				char ch = text[k];
				if (ch == '\r')
					continue;
				if (ch == '"')
					quoted = !quoted;
				if (ch == ';' && !quoted)
					break;                 // a comment, outside quotes
				code += ch;
			}
			std::vector<Tok> toks = tokenize(code);
			size_t drop = 0;
			while (dropLeadingOps && drop < toks.size() && toks[drop].kind == 'o')
				drop++;                    // stray special characters at the start of a line are skipped
			toks.erase(toks.begin(), toks.begin() + drop);
			if (!toks.empty())
				lines.push_back(std::move(toks));
			start = nl + 1;
		}
		return lines;
	}

	V* body(Frame& f)
	{
		if (f.kind == 'i')
			return f.inElse ? f.node->l[2] : f.node->l[1]->l.back()->l[1];
		return f.node;
	}

	// {"name", "locals", "body"} as a V, or nullptr
	V* compile(const std::string& text, std::string* name)
	{
		V* root = mk(V::LIST);
		std::vector<Frame> stack = { { 'r', root, false } };
		// Morrowind's own compiler takes a few malformed blocks: a stray endif is dropped, an elseif / else after an
		// endif that closed its chain too early reopens that chain, a second else is dead code
		bool haveClosed = false;
		size_t closedDepth = 0;
		Frame closed = { 'r', nullptr, false };
		for (auto& t : splitLines(text, true))
		{
			if (err)
				break;
			std::string word = t[0].kind == 'i' ? lower(t[0].text) : "";
			if ((word == "elseif" || word == "else") && stack.back().kind != 'i' && haveClosed && closedDepth == stack.size())
			{
				stack.push_back(closed);
				haveClosed = false;
			}
			V* b = body(stack.back());
			if (word == "begin")
				*name = t.size() > 1 ? t[1].text : "";
			else if (word == "end")
				break;
			else if (word == "short" || word == "long" || word == "float")
			{
				for (size_t k = 1; k < t.size(); k++)
					if (t[k].kind == 'i')
						locals.push_back({ word, lower(t[k].text) });
			}
			else if (word == "if")
			{
				size_t i = 1;
				V* cond = expr(t, i);
				V* node = list({ str("if"), list({ list({ cond, mk(V::LIST) }) }), nul() });
				b->l.push_back(node);
				stack.push_back({ 'i', node, false });
			}
			else if (word == "elseif")
			{
				size_t i = 1;
				V* cond = expr(t, i);
				if (stack.back().kind != 'i')        // OpenMW starts an if here (a warning)
				{
					V* node = list({ str("if"), list({ list({ cond, mk(V::LIST) }) }), nul() });
					b->l.push_back(node);
					stack.push_back({ 'i', node, false });
				}
				else
					stack.back().node->l[1]->l.push_back(list({ cond, mk(V::LIST) }));
			}
			else if (word == "else")
			{
				if (stack.back().kind != 'i')
					continue;                         // a stray else: ignored
				if (stack.back().inElse)
				{
					stack.pop_back();
					stack.push_back({ 'd', mk(V::LIST), false });     // a second else: never runs
					continue;
				}
				stack.back().node->l[2] = mk(V::LIST);
				stack.back().inElse = true;
			}
			else if (word == "endif")
			{
				if (stack.back().kind == 'd')
				{
					stack.pop_back();
					continue;
				}
				if (stack.back().kind != 'i')
					continue;                         // a stray endif
				closed = stack.back();
				stack.pop_back();
				closedDepth = stack.size();
				haveClosed = true;
			}
			else if (word == "while")
			{
				size_t i = 1;
				V* cond = expr(t, i);
				V* node = list({ str("while"), cond, mk(V::LIST) });
				b->l.push_back(node);
				stack.push_back({ 'w', node->l[2], false });
			}
			else if (word == "endwhile")
			{
				if (stack.back().kind != 'w')
					return fail("endwhile without while"), nullptr;
				stack.pop_back();
			}
			else if (word == "return")
				b->l.push_back(list({ str("ret") }));
			else
				b->l.push_back(statement(t));
		}
		return err ? nullptr : root;
	}
};

cJSON* toJson(const V* v)
{
	switch (v->k)
	{
	case V::INT: case V::FLT: return cJSON_CreateNumber(v->n);
	case V::STR: return cJSON_CreateString(v->s.c_str());
	case V::NUL: return cJSON_CreateNull();
	case V::LIST:
	{
		cJSON* a = cJSON_CreateArray();
		for (const V* k : v->l)
			cJSON_AddItemToArray(a, toJson(k));
		return a;
	}
	}
	return cJSON_CreateNull();
}

} // namespace

bool scriptKnownName(const std::string& name)
{
	std::string sig;
	char type;
	return name == "messagebox" || instrSignature(name, &sig) || funcInfo(name, &type, &sig);
}

bool scriptNeedsRef(const std::string& name)
{
	if (!scriptKnownName(name))
		return false;
	for (const char* n : kScNoRefNeeded)
		if (name == n)
			return false;
	return true;
}

cJSON* scriptCompile(const std::string& text, const ScriptCompileInput& in, std::string* error)
{
	Compiler c;
	c.globals = in.globals;
	c.locals = in.locals;
	std::string name;
	V* body = c.compile(text, &name);
	if (!body)
	{
		if (error)
			*error = c.errMsg;
		return nullptr;
	}
	cJSON* out = cJSON_CreateObject();
	cJSON_AddStringToObject(out, "name", name.c_str());
	cJSON* locals = cJSON_AddArrayToObject(out, "locals");
	for (auto& l : c.locals)
	{
		cJSON* pair = cJSON_CreateArray();
		cJSON_AddItemToArray(pair, cJSON_CreateString(l.first.c_str()));
		cJSON_AddItemToArray(pair, cJSON_CreateString(l.second.c_str()));
		cJSON_AddItemToArray(locals, pair);
	}
	cJSON_AddItemToObject(out, "body", toJson(body));
	return out;
}

cJSON* scriptCompileSnippet(const std::string& text, const ScriptCompileInput& in, std::string* error)
{
	ScriptCompileInput speaker = in;
	for (auto& l : speaker.locals)
		l.first = "unknown";                  // the speaker's variables: types not known here (as the converter)
	Compiler c;
	c.globals = speaker.globals;
	c.locals = speaker.locals;
	std::string name;
	V* body = c.compile(text, &name);
	if (!body)
	{
		if (error)
			*error = c.errMsg;
		return nullptr;
	}
	return toJson(body);
}

// OpenMW's console (mwgui/console.cpp, compiler/lineparser.cpp with expressions allowed): a line that starts with a
// number, a parenthesis, a sign, a variable, X.var, or a function that returns a value is an expression and its value
// is reported; anything else is one statement.
cJSON* scriptCompileConsole(const std::string& line, const ScriptCompileInput& in, char* valueType, std::string* error)
{
	Compiler c;
	c.globals = in.globals;
	c.locals = in.locals;
	*valueType = 0;
	auto lines = Compiler::splitLines(line, false);
	if (lines.empty())
	{
		if (error)
			*error = "nothing to run";
		return nullptr;
	}
	if (lines.size() > 1)
	{
		if (error)
			*error = "one line at a time";
		return nullptr;
	}
	const std::vector<Tok>& t = lines[0];
	const Tok& first = t[0];
	bool expression = false;
	std::string word = first.kind == 'i' ? lower(first.text) : "";
	std::string sig;
	char type;
	if (first.kind == 'n' || first.op("(") || first.op("-") || first.op("+"))
		expression = true;
	else if ((first.kind == 'i' || first.kind == 's') && t.size() > 2 && t[1].op("->"))
	{
		std::string func = t[2].kind == 'i' ? lower(t[2].text) : "";
		if (func == "messagebox" || instrSignature(func, &sig))
			expression = false;
		else if (funcInfo(func, &type, &sig))
			expression = true;
		else if (in.engineKnows && in.engineKnows(func))
			expression = startsWith(func, "get");
		else
		{
			if (error)
				*error = "unknown function or instruction: " + (func.empty() ? t[2].text : func);
			return nullptr;
		}
	}
	else if ((first.kind == 'i' || first.kind == 's') && t.size() > 2 && t[1].op(".") && t[2].kind == 'i')
		expression = true;
	else if (first.kind == 'i')
	{
		static const char* kBlockWords[] = { "if", "elseif", "else", "endif", "while", "endwhile", "begin", "end",
			"short", "long", "float", "return" };
		if (inList(kBlockWords, word))
		{
			if (error)
				*error = "'" + word + "' is not for the console";
			return nullptr;
		}
		if (word == "set")
			expression = false;
		else if (c.isLocal(word) || c.isGlobal(word))
			expression = true;
		else if (word == "messagebox" || instrSignature(word, &sig))
			expression = false;
		else if (funcInfo(word, &type, &sig))
			expression = true;
		else if (in.engineKnows && in.engineKnows(word))
			expression = startsWith(word, "get");
		else
		{
			if (error)
				*error = "unknown function or instruction: " + word;
			return nullptr;
		}
	}
	else
	{
		if (error)
			*error = "unexpected '" + first.text + "'";
		return nullptr;
	}
	if (expression)
	{
		size_t i = 0;
		V* e = c.expr(t, i);
		if (c.err)
		{
			if (error)
				*error = c.errMsg;
			return nullptr;
		}
		*valueType = c.ntype(e);
		return toJson(e);
	}
	V* s = c.statement(t);
	if (c.err)
	{
		if (error)
			*error = c.errMsg;
		return nullptr;
	}
	cJSON* block = cJSON_CreateArray();
	cJSON_AddItemToArray(block, toJson(s));
	return block;
}
