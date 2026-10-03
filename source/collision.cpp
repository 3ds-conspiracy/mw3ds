#include <algorithm>
#include "collision.h"

#include <cmath>

#include "planes.h"

// Reads n indices stored as u16 (width 2) or u32 (width 4), padded to 4 bytes; planes of
// differences in format 7
static bool readIndices(FILE* f, std::vector<u32>& out, u32 n, u32 width, bool planar)
{
	out.resize(n);
	if (width == 4)
	{
		bool ok = fread(out.data(), 4, n, f) == n;
		if (planar)
			unpackInts((u8*)out.data(), n, 4);
		return ok;
	}
	std::vector<u16> tmp(n);
	bool ok = fread(tmp.data(), 2, n, f) == n;
	if (planar)
		unpackInts((u8*)tmp.data(), n, 2);
	for (u32 i = 0; i < n; i++)
		out[i] = tmp[i];
	if (n & 1)
		fseek(f, 2, SEEK_CUR);
	return ok;
}

bool collisionRead(CollisionMesh& m, FILE* f, u32 version)
{
	bool packed = version >= 6, planar = version >= 7;
	u32 numVerts = 0, numTris = 0;
	if (fread(&numVerts, 4, 1, f) != 1)
		return false;
	m.verts.resize(numVerts * 3);
	if (packed)
	{
		// Cell format 6: welded vertices as s16 on one grid (offset + value * scale), narrow indices
		float offset[3], scale[3];
		fread(offset, 4, 3, f);
		fread(scale, 4, 3, f);
		std::vector<s16> q(numVerts * 3);
		fread(q.data(), 2, q.size(), f);
		if (q.size() & 1)
			fseek(f, 2, SEEK_CUR);
		if (planar)
		{
			unplane((u8*)q.data(), numVerts, 6);
			for (int k = 0; k < 3; k++)
				undelta16((u8*)q.data(), numVerts, 6, k * 2);
		}
		for (u32 i = 0; i < numVerts * 3; i++)
			m.verts[i] = offset[i % 3] + q[i] * scale[i % 3];
		u32 widths = 0;
		fread(&numTris, 4, 1, f);
		fread(&widths, 4, 1, f);      // byte 0: triangle index width, byte 1: grid list width
		readIndices(f, m.tris, numTris * 3, widths & 0xFF, planar);
		float grid[3];
		fread(grid, 4, 3, f);
		m.originX = grid[0];
		m.originY = grid[1];
		m.cellSize = grid[2];
		fread(&m.nx, 4, 1, f);
		fread(&m.ny, 4, 1, f);
		m.cellStart.resize(m.nx * m.ny + 1);
		fread(m.cellStart.data(), 4, m.cellStart.size(), f);
		if (planar)
			unpackInts((u8*)m.cellStart.data(), m.cellStart.size(), 4);
		return readIndices(f, m.cellTris, m.cellStart.back(), (widths >> 8) & 0xFF, planar);
	}
	fread(m.verts.data(), 4, numVerts * 3, f);
	fread(&numTris, 4, 1, f);
	m.tris.resize(numTris * 3);
	fread(m.tris.data(), 4, numTris * 3, f);
	float grid[3];
	fread(grid, 4, 3, f);
	m.originX = grid[0];
	m.originY = grid[1];
	m.cellSize = grid[2];
	fread(&m.nx, 4, 1, f);
	fread(&m.ny, 4, 1, f);
	m.cellStart.resize(m.nx * m.ny + 1);
	fread(m.cellStart.data(), 4, m.cellStart.size(), f);
	m.cellTris.resize(m.cellStart.back());
	return fread(m.cellTris.data(), 4, m.cellTris.size(), f) == m.cellTris.size();
}

// Calls fn(triangle index) once for every triangle in grid cells overlapping the XY box.
template <typename Fn>
static void forTrianglesIn(CollisionMesh& m, float x0, float y0, float x1, float y1, Fn fn)
{
	if (m.nx == 0)
		return;
	if (m.stamp.size() != m.tris.size() / 3)
		m.stamp.assign(m.tris.size() / 3, 0);
	m.queryId++;
	int gx0 = (int)floorf((x0 - m.originX) / m.cellSize), gx1 = (int)floorf((x1 - m.originX) / m.cellSize);
	int gy0 = (int)floorf((y0 - m.originY) / m.cellSize), gy1 = (int)floorf((y1 - m.originY) / m.cellSize);
	if (gx0 < 0) gx0 = 0;
	if (gy0 < 0) gy0 = 0;
	if (gx1 >= (int)m.nx) gx1 = m.nx - 1;
	if (gy1 >= (int)m.ny) gy1 = m.ny - 1;
	for (int gy = gy0; gy <= gy1; gy++)
		for (int gx = gx0; gx <= gx1; gx++)
		{
			u32 cell = gy * m.nx + gx;
			for (u32 i = m.cellStart[cell]; i < m.cellStart[cell + 1]; i++)
			{
				u32 t = m.cellTris[i];
				if (m.stamp[t] == m.queryId)
					continue;
				m.stamp[t] = m.queryId;
				fn(t);
			}
		}
}

static inline void sub(const float* a, const float* b, float* out)
{
	out[0] = a[0] - b[0]; out[1] = a[1] - b[1]; out[2] = a[2] - b[2];
}

static inline float dot(const float* a, const float* b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5)
static void closestOnTriangle(const float* p, const float* a, const float* b, const float* c, float* out)
{
	float ab[3], ac[3], ap[3];
	sub(b, a, ab); sub(c, a, ac); sub(p, a, ap);
	float d1 = dot(ab, ap), d2 = dot(ac, ap);
	if (d1 <= 0 && d2 <= 0) { out[0] = a[0]; out[1] = a[1]; out[2] = a[2]; return; }
	float bp[3];
	sub(p, b, bp);
	float d3 = dot(ab, bp), d4 = dot(ac, bp);
	if (d3 >= 0 && d4 <= d3) { out[0] = b[0]; out[1] = b[1]; out[2] = b[2]; return; }
	float vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0)
	{
		float v = d1 / (d1 - d3);
		for (int i = 0; i < 3; i++) out[i] = a[i] + v * ab[i];
		return;
	}
	float cp[3];
	sub(p, c, cp);
	float d5 = dot(ab, cp), d6 = dot(ac, cp);
	if (d6 >= 0 && d5 <= d6) { out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; return; }
	float vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0)
	{
		float w = d2 / (d2 - d6);
		for (int i = 0; i < 3; i++) out[i] = a[i] + w * ac[i];
		return;
	}
	float va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
	{
		float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		for (int i = 0; i < 3; i++) out[i] = b[i] + w * (c[i] - b[i]);
		return;
	}
	float denom = 1.0f / (va + vb + vc);
	float v = vb * denom, w = vc * denom;
	for (int i = 0; i < 3; i++) out[i] = a[i] + ab[i] * v + ac[i] * w;
}

bool collisionPushSphere(CollisionMesh& m, float c[3], float r, bool horizontalOnly)
{
	bool touched = false;
	forTrianglesIn(m, c[0] - r, c[1] - r, c[0] + r, c[1] + r, [&](u32 t) {
		const float* a = &m.verts[m.tris[t * 3 + 0] * 3];
		const float* b = &m.verts[m.tris[t * 3 + 1] * 3];
		const float* cc = &m.verts[m.tris[t * 3 + 2] * 3];
		float p[3], d[3];
		closestOnTriangle(c, a, b, cc, p);
		sub(c, p, d);
		// Overlap is decided in 3D; body spheres are then pushed only sideways
		if (dot(d, d) >= r * r)
			return;
		if (horizontalOnly)
			d[2] = 0;
		float dist2 = dot(d, d);
		if (dist2 < 1e-6f)
			return;
		float dist = sqrtf(dist2);
		float push = (r - dist) / dist;
		c[0] += d[0] * push;
		c[1] += d[1] * push;
		c[2] += d[2] * push;
		touched = true;
	});
	return touched;
}

bool collisionFloor(CollisionMesh& m, float x, float y, float zTop, float zBottom, float* zOut)
{
	bool found = false;
	float best = zBottom;
	forTrianglesIn(m, x, y, x, y, [&](u32 t) {
		const float* a = &m.verts[m.tris[t * 3 + 0] * 3];
		const float* b = &m.verts[m.tris[t * 3 + 1] * 3];
		const float* c = &m.verts[m.tris[t * 3 + 2] * 3];
		// Barycentric coordinates of (x, y) in the triangle's XY projection
		float det = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
		if (fabsf(det) < 1e-4f)
			return;   // vertical wall
		// Steeper than 60 degrees is a wall too (a leaning one would otherwise be climbed): the
		// normal's z (det is twice the XY-projected area) against its length
		float e1[3], e2[3];
		sub(b, a, e1);
		sub(c, a, e2);
		float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
		if (n[2] * n[2] < 0.25f * dot(n, n))
			return;
		float l1 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / det;
		float l2 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / det;
		float l3 = 1.0f - l1 - l2;
		if (l1 < 0 || l2 < 0 || l3 < 0)
			return;
		float z = l1 * a[2] + l2 * b[2] + l3 * c[2];
		if (z <= zTop && z >= best)
		{
			best = z;
			found = true;
		}
	});
	if (found)
		*zOut = best;
	return found;
}

bool collisionFootFloor(CollisionMesh& m, float x, float y, float reach, float zTop, float zBottom, float* zOut)
{
	static const float offs[5][2] = { { 0, 0 }, { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
	bool found = false;
	for (auto& o : offs)
	{
		float z;
		if (collisionFloor(m, x + o[0] * reach, y + o[1] * reach, zTop, zBottom, &z) && (!found || z > *zOut))
		{
			*zOut = z;
			found = true;
		}
	}
	return found;
}

// Segment a -> b against the mesh: nearest hit as a fraction of the way (Moller-Trumbore), walking
// the grid cells along the segment
static const float kEdgeSlack = 1e-3f;

bool collisionRaycast(CollisionMesh& m, const float a[3], const float b[3], float* tOut)
{
	if (m.nx == 0)
		return false;
	if (m.stamp.size() != m.tris.size() / 3)
		m.stamp.assign(m.tris.size() / 3, 0);
	m.queryId++;
	float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	float len = sqrtf(d[0] * d[0] + d[1] * d[1]);
	int steps = (int)(len / (m.cellSize * 0.5f)) + 1;
	float best = 2.0f;
	int lastCell = -1;
	for (int s = 0; s <= steps; s++)
	{
		float f = (float)s / steps;
		float x = a[0] + d[0] * f, y = a[1] + d[1] * f;
		// the cell under this point and its neighbours (the segment can clip a corner between steps)
		int gx = (int)floorf((x - m.originX) / m.cellSize), gy = (int)floorf((y - m.originY) / m.cellSize);
		if (gy * (int)m.nx + gx == lastCell)
			continue;
		lastCell = gy * m.nx + gx;
		for (int oy = -1; oy <= 1; oy++)
			for (int ox = -1; ox <= 1; ox++)
			{
				int cx = gx + ox, cy = gy + oy;
				if (cx < 0 || cy < 0 || cx >= (int)m.nx || cy >= (int)m.ny)
					continue;
				u32 cell = cy * m.nx + cx;
				for (u32 i = m.cellStart[cell]; i < m.cellStart[cell + 1]; i++)
				{
					u32 t = m.cellTris[i];
					if (m.stamp[t] == m.queryId)
						continue;
					m.stamp[t] = m.queryId;
					const float* p0 = &m.verts[m.tris[t * 3 + 0] * 3];
					const float* p1 = &m.verts[m.tris[t * 3 + 1] * 3];
					const float* p2 = &m.verts[m.tris[t * 3 + 2] * 3];
					float e1[3], e2[3], pv[3], tv[3], qv[3];
					sub(p1, p0, e1);
					sub(p2, p0, e2);
					pv[0] = d[1] * e2[2] - d[2] * e2[1]; pv[1] = d[2] * e2[0] - d[0] * e2[2]; pv[2] = d[0] * e2[1] - d[1] * e2[0];
					float det = dot(e1, pv);
					if (fabsf(det) < 1e-6f)
						continue;
					float inv = 1.0f / det;
					sub(a, p0, tv);
					// A little past the edges: a segment exactly along the seam between two triangles
					// (a round tower's rows of wall) otherwise slips between both
					float u = dot(tv, pv) * inv;
					if (u < -kEdgeSlack || u > 1.0f + kEdgeSlack)
						continue;
					qv[0] = tv[1] * e1[2] - tv[2] * e1[1]; qv[1] = tv[2] * e1[0] - tv[0] * e1[2]; qv[2] = tv[0] * e1[1] - tv[1] * e1[0];
					float v = dot(d, qv) * inv;
					if (v < -kEdgeSlack || u + v > 1.0f + kEdgeSlack)
						continue;
					float tt = dot(e2, qv) * inv;
					if (tt >= 0.0f && tt <= 1.0f && tt < best)
						best = tt;
				}
			}
		if (best <= f)
			break;               // nothing later along the segment can be nearer
	}
	if (best > 1.0f)
		return false;
	*tOut = best;
	return true;
}

void collisionHideTris(CollisionMesh& m, u32 first, u32 count, std::vector<u32>& saved)
{
	if (!saved.empty() || (first + count) * 3 > m.tris.size())
		return;
	if (!m.farVert)
	{
		m.farVert = m.verts.size() / 3;
		const float far[9] = { 0, 0, -1e6f, 1, 0, -1e6f, 0, 1, -1e6f };
		m.verts.insert(m.verts.end(), far, far + 9);
	}
	saved.assign(m.tris.begin() + first * 3, m.tris.begin() + (first + count) * 3);
	for (u32 k = first; k < first + count; k++)
	{
		m.tris[k * 3] = m.farVert;
		m.tris[k * 3 + 1] = m.farVert + 1;
		m.tris[k * 3 + 2] = m.farVert + 2;
	}
}

void collisionShowTris(CollisionMesh& m, u32 first, u32 count, std::vector<u32>& saved)
{
	if (saved.size() != count * 3 || (first + count) * 3 > m.tris.size())
		return;
	std::copy(saved.begin(), saved.end(), m.tris.begin() + first * 3);
	saved.clear();
}
