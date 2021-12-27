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

void skip_string(const char *&p, int size) {
	p += 1 + size;
}

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
	if(l > 10000)
		return std::string("");
	fflush(stdout);
	fflush(stderr);
	std::string s(p, 0, l);
	p += l;
	return s;
}

/*
 * Creates fake points for outline using outermost pins plus some margin.
 */
#define OUTLINE_MARGIN 20
void LAYFile::gen_outline() {
	// Determine board outline
	int minx =
	    std::min_element(pins.begin(), pins.end(), [](BRDPin a, BRDPin b) { return a.pos.x < b.pos.x; })->pos.x - OUTLINE_MARGIN;
	int maxx =
	    std::max_element(pins.begin(), pins.end(), [](BRDPin a, BRDPin b) { return a.pos.x < b.pos.x; })->pos.x + OUTLINE_MARGIN;
	int miny =
	    std::min_element(pins.begin(), pins.end(), [](BRDPin a, BRDPin b) { return a.pos.y < b.pos.y; })->pos.y - OUTLINE_MARGIN;
	int maxy =
	    std::max_element(pins.begin(), pins.end(), [](BRDPin a, BRDPin b) { return a.pos.y < b.pos.y; })->pos.y + OUTLINE_MARGIN;
	format.push_back({minx, miny});
	format.push_back({maxx, miny});
	format.push_back({maxx, maxy});
	format.push_back({minx, maxy});
	format.push_back({minx, miny});
}
#undef OUTLINE_MARGIN

/*
 * Updates element counts
 */
void LAYFile::update_counts() {
	num_parts  = parts.size();
	num_pins   = pins.size();
	num_format = format.size();
	num_nails  = nails.size();
}

bool LAYFile::verifyFormat(std::vector<char> &buf) {
	// first byte is format version
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
		uint32_t num_connections = 0; // TODO: count connections

		/* read board header */

		ENSURE(p - file_buf + 534 < buffer_size);
		s = read_string(p, 30);
		// unknown padding
		read_uint32(p);
		auto size_x = read_uint32(p);
		auto size_y = read_uint32(p);
		printf("Reading board %d '%s' %.4f x %.4f mm.\n", b, s.c_str(),
			   double(size_x) / 10000, double(size_y) / 10000);
		p += 7; // ground pane
		read_double(p); // active_grid_val
		read_double(p); // zoom
		printf("viewport_offset_x %d\n", read_uint32(p)); // viewport_offset_x
		printf("viewport_offset_y %d\n", read_uint32(p)); // viewport_offset_y
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
		printf("center_x %d\n", center_x);
		printf("center_y %d\n", center_y);
		p += 1; // multilayer flag

		uint32_t num_objects = read_int32(p);
		printf("Reading %d objects.\n", num_objects);

		for (uint32_t object = 0; object < num_objects; object++) {
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

		// generate default outline from board size
		if (format.size() == 0) {
			// should we offset by center_x/y?
			format.push_back({0, 0});
			format.push_back({size_x, 0});
			format.push_back({size_x, -size_y});
			format.push_back({0, -size_y});
			format.push_back({0, 0});
		}

		/* TODO: read connections */
		for (uint32_t connection = 0; connection < num_connections; connection++) {
			uint32_t len = read_uint32(p);
			p += len * 4;
		}
	}

	/* read trailer */
	read_uint32(p); // active_board_tab
	s = read_string(p, 100);
	printf("Project name: '%s'\n", s.c_str());
	s = read_string(p, 100);
	printf("Project author: '%s'\n", s.c_str());
	s = read_string(p, 100);
	printf("Project company: '%s'\n", s.c_str());
	s = read_hugestring(p);
	printf("Comment: '%s'\n", s.c_str());

	//gen_outline(); // We haven't figured out how to get the outline yet

	update_counts(); // FIXME: useless?

	valid = true; // FIXME: better way to ensure that?
}

bool LAYFile::ReadObject(const char *&p, const char *file_buf, bool isTextChild, int indent)
{
	uint32_t count = 0;
	const char *o = p;
	//printf("p = %p\n", p);
	/* read object header */
	uint8_t object_type = /*read_uint32(p);*/*p++;
	auto origin_x = read_float(p);
	auto origin_y = read_float(p);
	// for circles
	auto r_out = read_float(p);
	auto r_in = read_float(p);
	uint32_t line_width = read_uint32(p);
	p++; // padding ?
	uint8_t layer = *p++;
	uint8_t tht_shape = *p++;

	p+=4; // padding ?
	uint16_t component_id = read_uint16(p);
	printf("%.*s@ 0x%lx ### object_type %d at %fmm x %fmm component_id %d\n", indent, TABSTABS, o - file_buf, object_type, origin_x / 10000, origin_y / 10000, component_id);
	printf("tht_shape %d\n", tht_shape);
	p++; // unknown
	read_int32(p); // start_angle; also th_style[4]
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
	printf("padding @ 0x%lx\n", p - file_buf);
	p += 18; // unknown

	//printf("headerlen: %ld\n", p - o);
	if (!isTextChild) {
		std::string s;
		s = read_hugestring(p);
		printf("%.*s@ 0x%lx (%d) text: '%s'\n", indent, TABSTABS, p - file_buf, object_type, s.c_str());
		s = read_hugestring(p);
		printf("%.*s@ 0x%lx (%d) marker: '%s'\n", indent, TABSTABS, p - file_buf, object_type, s.c_str());
		uint32_t groups = read_uint32(p);
		printf("%.*s@ 0x%lx (%d) %d groups:\n", indent, TABSTABS, p - file_buf, object_type, groups);
		for (uint32_t i = 0; i < groups; i++) {
			uint32_t group = read_uint32(p);
			printf("%.*s%d\n", indent+2, TABSTABS, group);
		}
	}

	switch (object_type) {
		case 2: // OBJ_THT_PAD
		case 8: // OBJ_SMD_PAD
		{
			//num_connections++;
//#if 0
			BRDPin pin;

			pin.part = 1;//component_id;
			pin.probe = 0;
			pin.net = "";
			pin.pos.x = (int)origin_x;
			pin.pos.y = (int)origin_y;
			pins.push_back(pin);
//#endif
#if 0
			BRDNail nail;
			nail.probe = 0;
			nail.pos.x = (int)origin_x;
			nail.pos.y = (int)origin_y;
			nail.side  = 0;
			nail.net   = "";
			nails.push_back(nail);
#endif
			break;
		}
		case 4: // OBJ_POLY
		case 6: // OBJ_LINE
			break;
		case 5: { // OBJ_CIRCLE
			return true; // no points list
		}
		case 7: { // OBJ_TEXT
			count = read_uint32(p);
			printf("%.*s@ 0x%lx Reading %d sub-objects\n", indent, TABSTABS, p - file_buf, count);
			for (uint32_t i = 0; i < count; i++) {
				printf("%.*s@ 0x%lx Reading sub-object %d of %d\n", indent, TABSTABS, p - file_buf, i+1, count);
				ENSURE(count < 1000); //XXX
				if (count > 1000)
					return false;
				ReadObject(p, file_buf, true, indent + 1);
			}
			if (tht_shape == 1) {
				printf("%.*sReading component\n", indent, TABSTABS);
				// header
				float off_x = read_float(p), off_y = read_float(p);
				uint8_t center_mode = *p++;
				double rotation = read_double(p);

				std::string s;
				s = read_hugestring(p);
				printf("%.*s@ 0x%lx %d package: '%s'\n", indent, TABSTABS, p - file_buf, object_type, s.c_str());
				s = read_hugestring(p);
				printf("%.*s@ 0x%lx %d comment: '%s'\n", indent, TABSTABS, p - file_buf, object_type, s.c_str());
				p++; // use
			}
			return true; // no points list
		}
		default:
			fprintf(stderr, "Unknown object type %d!\n", object_type);
			return false;
	}

	// now we have a list of points
	count = read_uint32(p);
	printf("%.*s@ 0x%lx %d points:\n", indent, TABSTABS, p - file_buf, count);
	for (uint32_t i = 0; i < count; i++) {
		float x = read_float(p), y = read_float(p);
		printf("%.*s%f x %f\n", indent+2, TABSTABS, x / 10000, y / 10000);
	}
	return true;
}
