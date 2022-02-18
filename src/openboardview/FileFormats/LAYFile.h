#pragma once

#include "BRDFile.h"

struct LAYFile : public BRDFile {
	LAYFile(std::vector<char> &buf);
	~LAYFile() {
		free(file_buf);
	}

	bool ReadObject(const char *&p, const char *file_buf, bool isTextChild = false, int indent = 0);
	void outline_order_segments(std::vector<BRDPoint> &format);

	static bool verifyFormat(std::vector<char> &buf);
  private:
	void gen_outline();
	void update_counts();

	uint32_t num_connections;
	std::vector <int> objects;

	/* used to reconstruct a usable outline from unordered polylines and arcs */
	std::vector<std::vector <BRDPoint>> outline_segments;
};
