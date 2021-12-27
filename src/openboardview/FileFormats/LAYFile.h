#pragma once

#include "BRDFile.h"

struct LAYFile : public BRDFile {
	LAYFile(std::vector<char> &buf);
	~LAYFile() {
		free(file_buf);
	}

	bool ReadObject(const char *&p, const char *file_buf, bool isTextChild = false, int indent = 0);

	static bool verifyFormat(std::vector<char> &buf);
  private:
	void gen_outline();
	void update_counts();
};
