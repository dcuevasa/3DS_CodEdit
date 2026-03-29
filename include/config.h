#ifndef _3D_SHELL_CONFIG_H
#define _3D_SHELL_CONFIG_H

#include <3ds.h>
#include <string>

typedef struct {
	int sort = 0;
	bool dev_options = false;
	bool dark_theme = false;
	std::string cwd;
	std::string git_remote_url;
	std::string git_default_branch;
	std::string git_pat;
} config_t;

extern config_t cfg;

namespace Config {
	int Save(config_t config);
	int Load(void);
}

#endif
