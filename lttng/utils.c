/*
 * Copyright (c)  2011 David Goulet <david.goulet@polymtl.ca>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdlib.h>

#include <lttng/lttng.h>

#include "conf.h"

/*
 *  get_config_file_path
 *
 *  Return absolute path to the configuration file.
 */
char *get_config_file_path(void)
{
	char *alloc_path, *path = NULL;

	/* Get path to config directory */
	alloc_path = config_get_default_path();
	if (alloc_path == NULL) {
		goto error;
	}

	/* Get path to config file */
	path = config_generate_dir_path(alloc_path);
	if (path == NULL) {
		goto free_alloc_path;
	}

free_alloc_path:
	free(alloc_path);
error:
	return path;
}

/*
 *  get_session_name
 *
 *  Return allocated string with the session name found in the config
 *  directory.
 */
char *get_session_name(void)
{
	char *path, *session_name = NULL;

	/* Get path to config file */
	path = get_config_file_path();
	if (path == NULL) {
		goto error;
	}

	/* Get session name from config */
	session_name = config_read_session_name(path);
	if (session_name == NULL) {
		goto free_path;
	}

free_path:
	free(path);
error:
	return session_name;
}

/*
 *  set_session_name
 *
 *  Get session name and set it for the lttng control lib.
 */
int set_session_name(void)
{
	int ret;
	char *session_name;

	session_name = get_session_name();
	if (session_name == NULL) {
		ret = -1;
		goto error;
	}

	lttng_set_session_name(session_name);
	free(session_name);

	ret = 0;

error:
	return ret;
}