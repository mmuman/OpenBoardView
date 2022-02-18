#include "LAYFile.h"

#include "utils.h"
#include <algorithm>
#include <cctype>
#include <clocale>
#include <cstdint>
#include <cstring>

/*
 * References:
 * https://github.com/sergey-raevskiy/xlay
 */

/* layer identifiers */
enum {
	LAYER_C1 = 1, // top copper
	LAYER_S1, // top silkscreen
	LAYER_C2, // bottom copper
	LAYER_S2, // bottom silkscreen
	LAYER_I1, // inner (close to top)
	LAYER_I2, // inner (close to bottom)
	LAYER_O, // outline
};

/* object types */
enum {
	OBJ_THT_PAD = 2,
	OBJ_POLY = 4,
	OBJ_CIRCLE,
	OBJ_LINE,
	OBJ_TEXT,
	OBJ_SMD_PAD
};


// TODO: check for endianness and pointer alignment

uint16_t read_uint16(const char *&p) {
#warning FIXME: Support big endian host
	uint16_t v = *(reinterpret_cast<const uint16_t *>(p));
	p += sizeof(v);
	return v;
}

int32_t read_int32(const char *&p) {
#warning FIXME: Support big endian host
	int32_t v = *(reinterpret_cast<const int32_t *>(p));
	p += sizeof(v);
	return v;
}

uint32_t read_uint32(const char *&p) {
#warning FIXME: Support big endian host
	uint32_t v = *(reinterpret_cast<const uint32_t *>(p));
	p += sizeof(v);
	return v;
}

float read_float(const char *&p) {
#warning FIXME: Support big endian host
	float v = *(reinterpret_cast<const float *>(p));
	p += sizeof(v);
	return v;
}

double read_double(const char *&p) {
#warning FIXME: Support big endian host
	double v = *(reinterpret_cast<const double *>(p));
	p += sizeof(v);
	return v;
}

// The format uses Pascal strings but padded to fixed sizes.
std::string read_string(const char *&p, int size) {
	uint8_t l = *p++;
	std::string s(p, 0, l);
	p += size;
	return s;
}

// Text object uses Pascal strings but with uint32 length prefix and no preallocated size.
std::string read_hugestring(const char *&p) {
	uint32_t l = read_uint32(p);
	ENSURE(l < 10000);
	// something probably went wrong
	if(l > 10000)
		return std::string("");
	fflush(stdout);
	fflush(stderr);
	std::string s(p, 0, l);
	p += l;
	return s;
}

// We approximate arcs with segments every 15 deg
inline void point_on_arc(BRDPoint &p, BRDPoint &center, uint32_t angle, uint32_t radius) {
	float a = (float)(angle / 1000) * 2 * M_PI / 360;
	p.x = center.x + cos(a) * radius;
	p.y = center.y + sin(a) * radius;
}

// TODO: figure out delta from line width?
#define PROXIMITY_DELTA 1000

// Due to approximation when calculating arc endpoints we prefer to not compare strictly
inline bool close_enough(BRDPoint &a, BRDPoint &b, uint32_t delta) {
	return abs(a.x - b.x) < delta && abs(a.y - b.y) < delta;
}

// Collect the segments from the outline layer, and try to join them to form closed outlines
void LAYFile::outline_order_segments(std::vector<BRDPoint> &format) {
	std::vector<std::vector<BRDPoint>>::iterator sit;

	// we probably don't need that many iterations but well, it just works.
	for (uint32_t i = 0; i < outline_segments.size(); i++) {
		// for each polyline we try to find another one starting at its end
		for (sit = outline_segments.begin(); sit != outline_segments.end(); sit++) {
			std::vector<BRDPoint> &a = *sit;
			if (a.size() == 0)
				continue;

			std::vector<std::vector<BRDPoint>>::iterator nit;
			for (nit = outline_segments.begin(); nit != outline_segments.end(); nit++) {
				if (nit == sit)
					continue;

				std::vector<BRDPoint> &b = *nit;
				if (b.size() == 0)
					continue;
				BRDPoint p = b.back();

				// the ends touch
				if (close_enough(a.back(), p, PROXIMITY_DELTA))
					std::reverse(b.begin(), b.end());

				b = *nit;
				p = b.front();
				if (!close_enough(a.back(), p, PROXIMITY_DELTA))
					continue;

				// they seem to go along, join them
				a.reserve(a.size() + b.size());
				// skip duplicated point
				if (p.x == a.back().x && p.y == a.back().y)
					a.insert(a.end(), b.begin() + 1, b.end());
				else
					a.insert(a.end(), b.begin(), b.end());
				b.clear();
			}
		}
	}

	for (sit = outline_segments.begin(); sit != outline_segments.end(); sit++) {
		std::vector<BRDPoint> &seg = *sit;

		// skip empty vectors, and single points
		if (seg.size() < 2)
			continue;

		BRDPoint p = seg.back(), p1 = seg.front();
		// skip non-closed shapes
		if (p.x != p1.x || p.y != p1.y)
			continue;

		// append segments to outline
		format.reserve(format.size() + seg.size());
		format.insert(format.end(), seg.begin(), seg.end());
	}
}

/*
 * Updates element counts
 */
void LAYFile::update_counts() {
	num_parts  = parts.size();
	num_pins   = pins.size();
	num_format = format.size();
	num_nails  = nails.size();
	fprintf(stderr, "%d parts %d pins %d formats %d nails\n", num_parts, num_pins, num_format, num_nails);
}

bool LAYFile::verifyFormat(std::vector<char> &buf) {
	// first byte supposedly is format version
	static const uint8_t magic[] = {0x06, 0x33, 0xAA , 0xFF};
	if (buf.size() < 8)
		return false;
	const char *p = buf.data(); // XXX: is this std? use calloc+std:copy instead?
	if (memcmp(p+1, magic+1, 3) || p[0] > 6)
		return false;
	p += 4;
	// 100 boards in a single file would already be quite insane
	return read_uint32(p) < 100;
}

#define TABSTABS "\t\t\t\t\t\t\t\t"

LAYFile::LAYFile(std::vector<char> &buf) {
	auto buffer_size = buf.size();
	std::string s;

	// XXX: use buf.data() ?
	ENSURE(buffer_size > 4);
	file_buf             = (char *)calloc(1, buffer_size);
	ENSURE(file_buf != nullptr);

	std::copy(buf.begin(), buf.end(), file_buf);

	const char *p = file_buf; // Not quite C++ but it's easier to work with a raw pointer here

	p += 4;
	uint32_t boards = read_uint32(p);
	if (boards > 1) {
		fprintf(stderr, "Unsupported: more than 1 board (%d boards found)\n", boards);
		// TODO: support more than 1 board
		// possibly prefix names with Bnn_ or something, and offset layouts by previous board width.
	}

	for (uint32_t b = 0; b < 1/*boards*/; b++) {
		num_connections = 0;

		/* read board header */

		ENSURE(p - file_buf + 534 < buffer_size);
		s = read_string(p, 30);
		// unknown padding
		read_uint32(p);
		auto size_x = read_uint32(p);
		auto size_y = read_uint32(p);
		fprintf(stderr, "Reading board %d '%s' %.4f x %.4f mm.\n", b, s.c_str(),
			   double(size_x) / 10000, double(size_y) / 10000);
		p += 7; // ground pane
		read_double(p); // active_grid_val
		read_double(p); // zoom
		fprintf(stderr, "viewport_offset_x %d\n", read_uint32(p)); // viewport_offset_x
		fprintf(stderr, "viewport_offset_y %d\n", read_uint32(p)); // viewport_offset_y
		read_uint32(p); // active layer (or byte + 3 padding?)
		p += 7; // visible layers
		p += 1; // show_scanned_copy_top
		p += 1; // show_scanned_copy_bottom
		s = read_string(p, 200); // scanned_copy_top_path
		s = read_string(p, 200); // scanned_copy_bottom_path
		read_uint32(p); // DPI top
		read_uint32(p); // DPI bottom
		read_uint32(p); // shiftx_top
		read_uint32(p); // shifty_top
		read_uint32(p); // shiftx_bottom
		read_uint32(p); // shifty_bottom
		read_uint32(p); // unknown
		read_uint32(p); // unknown
		auto center_x = read_int32(p), center_y = read_int32(p);
		fprintf(stderr, "center_x %d\n", center_x);
		fprintf(stderr, "center_y %d\n", center_y);
		p += 1; // multilayer flag

		uint32_t num_objects = read_int32(p);
		fprintf(stderr, "Reading %d objects.\n", num_objects);

		for (uint32_t object = 0; object < num_objects; object++) {
			//XXX:objects.push_back(-1);
			if (!ReadObject(p, file_buf))
				return;
		}

		// Add dummy parts for orphan pins on both sides
		BRDPart part;
		part.name          = "...";
		part.mounting_side = BRDPartMountingSide::Both; // FIXME: Both sides?
		part.part_type     = BRDPartType::ThroughHole;
		part.end_of_pins   = 0; // Unused
		parts.push_back(part);

		// generate outline from the segments we collected out of order so far
		outline_order_segments(format);

		// generate default outline from board size if none was found
		if (format.size() == 0) {
			// should we offset by center_x/y?
			format.push_back({0, 0});
			format.push_back({(int)size_x, 0});
			format.push_back({(int)size_x, -(int)size_y});
			format.push_back({0, -(int)size_y});
			format.push_back({0, 0});
		}

		/* TODO: handle connections (but most boards don't have them filled anyway) */
		fprintf(stderr, "Reading %d connections:\n", num_connections);
		for (uint32_t connection = 0; connection < num_connections; connection++) {
			uint32_t len = read_uint32(p);
			fprintf(stderr, "Connection %d: %d to read:\n", connection, len);
			for (uint32_t c = 0; c < len; c++) {
				uint32_t conn = read_uint32(p);
				fprintf(stderr, "\t0x%08x %d\n", conn, conn);
			}
		}
	}

	/* read trailer */
	read_uint32(p); // active_board_tab
	s = read_string(p, 100);
	fprintf(stderr, "Project name: '%s'\n", s.c_str());
	s = read_string(p, 100);
	fprintf(stderr, "Project author: '%s'\n", s.c_str());
	s = read_string(p, 100);
	fprintf(stderr, "Project company: '%s'\n", s.c_str());
	s = read_hugestring(p);
	fprintf(stderr, "Comment: '%s'\n", s.c_str());

	update_counts(); // FIXME: useless?

	valid = true; // FIXME: better way to ensure that?
}

bool LAYFile::ReadObject(const char *&p, const char *file_buf, bool isTextChild, int indent)
{
	uint32_t count = 0;
	const char *o = p;
	BRDPart *part = nullptr;

	/* read object header */
	uint8_t object_type = *p++;
	auto origin_x = read_float(p);
	auto origin_y = read_float(p);
	// for circles
	auto r_out = read_float(p);
	auto r_in = read_float(p);
	uint32_t line_width = read_uint32(p);
	uint32_t end_angle = line_width; // for circles
	p++; // padding ?
	uint8_t layer = *p++;
	uint8_t tht_shape = *p++;
	p+=4; // padding ?
	uint16_t component_id = read_uint16(p);
	fprintf(stderr, "%.*s@ 0x%lx ### L:%d object_type %d at %fmm x %fmm r_in %f r_out %f component_id %d line_width %d tht_shape %d\n", indent, TABSTABS, o - file_buf, layer, object_type, origin_x / 10000, origin_y / 10000, r_in, r_out, component_id, line_width, tht_shape);
	p++; // unknown
	int32_t start_angle = read_int32(p); // start_angle; also th_style[4]
	p += 5; // unknown
	p++; // th_style_custom ; also fill
	read_int32(p); // ground_distance
	p += 5; // unknown
	p++; // thermobarier
	p++; // flip_vertical
	p++; // cutoff
	read_int32(p); // thsize ; rotation
	p++; // metalisation
	p++; // soldermask
	fprintf(stderr, "padding @ 0x%lx\n", p - file_buf);
	p += 18; // unknown

	if (component_id) {
		for (size_t i = parts.size(); i <= (size_t)component_id; i++) {
			BRDPart bp;
			bp.name = "...";
			parts.push_back(bp);
		}
		part = &parts[component_id];
	}

	//fprintf(stderr, "headerlen: %ld\n", p - o);
	std::string text = "";
	std::string marker;
	if (!isTextChild) {
		text = read_hugestring(p);
		fprintf(stderr, "%.*s@ 0x%lx (%d) text: '%s'\n", indent, TABSTABS, p - file_buf, object_type, text.c_str());
		marker = read_hugestring(p);
		fprintf(stderr, "%.*s@ 0x%lx (%d) marker: '%s'\n", indent, TABSTABS, p - file_buf, object_type, marker.c_str());
		uint32_t groups = read_uint32(p);
		fprintf(stderr, "%.*s@ 0x%lx (%d) %d groups:\n", indent, TABSTABS, p - file_buf, object_type, groups);
		for (uint32_t i = 0; i < groups; i++) {
			uint32_t group = read_uint32(p);
			fprintf(stderr, "%.*s%d\n", indent+2, TABSTABS, group);
		}
	}

	switch (object_type) {
		case OBJ_THT_PAD:
		case OBJ_SMD_PAD:
		{
			num_connections++;
			BRDPin pin;

			pin.part = component_id + 1;
			pin.probe = 0;
			fprintf(stderr, "PIN: MARKER: '%s'\n", marker.c_str());
			pin.net = strdup(marker.c_str()); //XXX:LEAK

			// TODO: check for tht_shape
			pin.radius = (r_in + r_out) / (2 * 10000);
			pin.pos.x = (int)origin_x;
			pin.pos.y = (int)origin_y;
			pins.push_back(pin);
			break;
		}
		case OBJ_POLY:
			if (layer == LAYER_O && !isTextChild) {
				fprintf(stderr, "%.*s@ 0x%lx OUTLINE: POLY\n", indent, TABSTABS, p - file_buf);
			}
		case OBJ_LINE:
			break;
		case OBJ_CIRCLE: {
			if (layer == LAYER_O) {
				// used either as complete cercles (holes) or arcs to join segments
				fprintf(stderr, "%.*sOutline Circle: %f x %f rout %f rin %f sa %d ea %d\n", indent, TABSTABS, origin_x, origin_y, r_out,  r_in, start_angle, end_angle);

				std::vector<BRDPoint> segments;
				uint32_t angle = start_angle;
				BRDPoint o({(int)origin_x, (int)origin_y}), p;
				float radius = (r_out + r_in) / 2;
				point_on_arc(p, o, angle, radius);
				segments.push_back({p.x, p.y});
				// for full circles, and some arcs that start at 270 and end at 0
				if (end_angle <= start_angle)
					end_angle += 360000;
				angle = (start_angle / 15000 + 1) * 15000;

				for (; angle < end_angle; angle += 15000) {
					point_on_arc(p, o, angle, radius);
					fprintf(stderr, "%.*s ARC: %d x %d %lu %f\n", indent+2, TABSTABS, p.x, p.y, angle, radius);
					segments.push_back({p.x, p.y});
				}
				point_on_arc(p, o, end_angle, radius);
				segments.push_back({p.x, p.y});
				outline_segments.push_back(segments);
			}
			return true; // no points list
		}
		case OBJ_TEXT: {
			count = read_uint32(p);
			fprintf(stderr, "%.*s@ 0x%lx Reading %d sub-objects\n", indent, TABSTABS, p - file_buf, count);
			for (uint32_t i = 0; i < count; i++) {
				fprintf(stderr, "%.*s@ 0x%lx Reading sub-object %d of %d\n", indent, TABSTABS, p - file_buf, i+1, count);
				ENSURE(count < 1000); //XXX
				if (count > 1000)
					return false;
				ReadObject(p, file_buf, true, indent + 1);
			}
			if (tht_shape == 1) {
				// header
				float off_x = read_float(p), off_y = read_float(p); // seems unused
				uint8_t center_mode = *p++;
				double rotation = read_double(p);
				fprintf(stderr, "%.*sReading component off_x/y %f x %f center_mode %d rotation %f\n", indent, TABSTABS, off_x, off_y, center_mode, rotation);

				std::string s;
				s = read_hugestring(p);
				fprintf(stderr, "%.*s@ 0x%lx %d package: '%s'\n", indent, TABSTABS, p - file_buf, object_type, s.c_str());
				s = read_hugestring(p);
				fprintf(stderr, "%.*s@ 0x%lx %d comment: '%s'\n", indent, TABSTABS, p - file_buf, object_type, s.c_str());
				uint8_t use = *p++; // use
				fprintf(stderr, "%.*s@ 0x%lx %d use: '%d'\n", indent, TABSTABS, p - file_buf, use);

				// we shouldn't have component_id == 0 here
				ENSURE(component_id);
				ENSURE(part);
				if (part) {
					if (text.size())
						part->name = strdup(text.c_str()); // XXX:LEAK
					//fprintf(stderr, "PART: %s\n", part->name);
				}
			}
			if (tht_shape == 2) { // that's the component value field
				ENSURE(component_id);
				ENSURE(part);
				if (part) {
					if (text.size())
						part->mfgcode = text.c_str();
				}
			}
			return true; // no points list
		}
		default:
			fprintf(stderr, "Unknown object type %d!\n", object_type);
			return false;
	}

	// now we have a list of points
	count = read_uint32(p);
	fprintf(stderr, "%.*s@ 0x%lx %d points:\n", indent, TABSTABS, p - file_buf, count);
	std::vector<BRDPoint> segments;
	int min_x, min_y, max_x, max_y;
	for (uint32_t i = 0; i < count; i++) {
		float x = read_float(p), y = read_float(p);
		fprintf(stderr, "%.*s%f x %f\n", indent+2, TABSTABS, x / 10000, y / 10000);
		segments.push_back({(int)x, (int)y});
		if (i == 0) {
			min_x = max_x = x;
			min_y = max_y = y;
		}
		min_x = std::min(min_x, (int)x);
		min_y = std::min(min_y, (int)y);
		max_x = std::max(max_x, (int)x);
		max_y = std::max(max_y, (int)y);
	}

	// pass the outline
	// we skip text object children since they are letters and not boxes
	if (layer == LAYER_O && object_type == OBJ_LINE && !isTextChild) {
		fprintf(stderr, "%.*s@ 0x%lx OUTLINE: %d points:\n", indent, TABSTABS, p - file_buf, count);
		outline_segments.push_back(segments);
	}
	if (part) {
		ENSURE(component_id);
		ENSURE(part);
		if (part->p1.x == 0 && part->p1.y == 0 && part->p2.x == 0 && part->p2.y == 0) {
			fprintf(stderr, "INIT p1 p2\n");
			part->p1 = {min_x, min_y};
			part->p2 = {max_x, max_y};
		}
		part->p1 = {std::min(part->p1.x, min_x), std::min(part->p1.y, min_y)};
		part->p2 = {std::max(part->p2.x, max_x), std::max(part->p2.y, max_y)};
	}
	return true;
}
