/*
 * download.c
 * file download helper functions
 *
 * Copyright (c) 2012-2019 Nikias Bassen. All Rights Reserved.
 * Copyright (c) 2012-2013 Martin Szulecki. All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "download.h"
#include "common.h"

typedef struct {
	int length;
	char* content;
} curl_response;

static size_t download_write_buffer_callback(char* data, size_t size, size_t nmemb, curl_response* response) {
	size_t total = size * nmemb;
	if (total != 0) {
		response->content = realloc(response->content, response->length + total + 1);
		memcpy(response->content + response->length, data, total);
		response->content[response->length + total] = '\0';
		response->length += total;
	}
	return total;
}

int download_to_buffer(const char* url, void** buf, size_t* length)
{
	int res = 0;
	CURL* handle = curl_easy_init();
	if (handle == NULL) {
		logger(LL_ERROR, "could not initialize CURL\n");
		return -1;
	}

	curl_response response;
	response.length = 0;
	response.content = malloc(1);
	response.content[0] = '\0';

	if (log_level >= LL_DEBUG)
		curl_easy_setopt(handle, CURLOPT_VERBOSE, 1);

	/* disable SSL verification to allow download from untrusted https locations */
	curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0);

	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, (curl_write_callback)&download_write_buffer_callback);
	curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response);
	if (strncmp(url, "https://api.ipsw.me/", 20) == 0) {
		curl_easy_setopt(handle, CURLOPT_USERAGENT, USER_AGENT_STRING " idevicerestore/" PACKAGE_VERSION);
	} else {
		curl_easy_setopt(handle, CURLOPT_USERAGENT, USER_AGENT_STRING);
	}
	curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt(handle, CURLOPT_URL, url);

	curl_easy_perform(handle);
	curl_easy_cleanup(handle);

	if (response.length > 0) {
		*length = response.length;
		*buf = response.content;
	} else {
		res = -1;
	}

	return res;
}

#if LIBCURL_VERSION_NUM >= 0x072000
static int download_progress(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
#else
static int download_progress(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow)
#endif
{
	double p = ((double)dlnow / (double)dltotal);

	set_progress('DNLD', p);

	if (global_quit_flag > 0) {
		return 1;
	}

	return 0;
}

#if defined(__ANDROID__) || defined(HV_SAFE_DOWNLOAD)
/* Henge (Android and the tests; the iOS build defines neither, so it keeps the upstream function below unchanged).
   Upstream writes straight into the final file and ignores curl_easy_perform's result: a download cut off half way is reported as success and its
   partial file stays. Here the data goes to a unique temporary file next to the target (<name>.hvpart.XXXXXX, locked with flock while in use); only a
   transfer that is complete under every check below is renamed onto the target. On any failure the temporary file is removed and an existing target is
   left as it was. Orphans from a killed process are removed at the start of the next download (a file whose lock can be taken has no live owner).
   Success needs: curl OK, HTTP status 200, at least one byte, the byte count equal to Content-Length when the response has one, and no write, flush,
   fsync, close or rename error. A response without Content-Length (chunked, or read until close) cannot be checked for length here: a connection that
   the peer ends early can look complete, so such files still need the caller's own hash check. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define HV_PART ".hvpart."

static void hv_remove_orphans(const char* filename)
{
	char dir[1024];
	const char* slash = strrchr(filename, '/');
	const char* base = slash ? slash + 1 : filename;
	if (slash) {
		size_t n = (size_t)(slash - filename);
		if (n >= sizeof(dir)) return;
		memcpy(dir, filename, n);
		dir[n] = 0;
	} else {
		strcpy(dir, ".");
	}
	char prefix[600];
	snprintf(prefix, sizeof(prefix), "%s" HV_PART, base);
	size_t pl = strlen(prefix);
	DIR* d = opendir(dir);
	if (!d) return;
	struct dirent* de;
	while ((de = readdir(d)) != NULL) {
		if (strncmp(de->d_name, prefix, pl) != 0) continue;
		char path[1700];
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		int fd = open(path, O_RDONLY);
		if (fd < 0) continue;
		if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
			unlink(path);
			logger(LL_WARNING, "Removed a leftover partial download '%s'\n", path);
		}
		close(fd);
	}
	closedir(d);
}

int download_to_file(const char* url, const char* filename, int enable_progress)
{
	char tmp[1700];
	if (snprintf(tmp, sizeof(tmp), "%s" HV_PART "XXXXXX", filename) >= (int)sizeof(tmp)) {
		logger(LL_ERROR, "Path too long: '%s'\n", filename);
		return -1;
	}
	hv_remove_orphans(filename);
	int fd = mkstemp(tmp);
	if (fd < 0) {
		logger(LL_ERROR, "Cannot create a temporary file for '%s': %s\n", filename, strerror(errno));
		return -1;
	}
	flock(fd, LOCK_EX | LOCK_NB);
	fchmod(fd, 0644);
	FILE* f = fdopen(fd, "wb");
	if (!f) {
		close(fd);
		unlink(tmp);
		return -1;
	}
	CURL* handle = curl_easy_init();
	if (handle == NULL) {
		logger(LL_ERROR, "Could not initialize CURL\n");
		fclose(f);
		unlink(tmp);
		return -1;
	}

	if (log_level >= LL_DEBUG)
		curl_easy_setopt(handle, CURLOPT_VERBOSE, 1);
	curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0);
	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, NULL);
	curl_easy_setopt(handle, CURLOPT_WRITEDATA, f);
	if (enable_progress > 0) {
		register_progress('DNLD', "Downloading");
#if LIBCURL_VERSION_NUM >= 0x072000
		curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, (curl_progress_callback)&download_progress);
#else
		curl_easy_setopt(handle, CURLOPT_PROGRESSFUNCTION, (curl_progress_callback)&download_progress);
#endif
	}
	curl_easy_setopt(handle, CURLOPT_NOPROGRESS, enable_progress > 0 ? 0: 1);
	curl_easy_setopt(handle, CURLOPT_USERAGENT, USER_AGENT_STRING);
	curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt(handle, CURLOPT_URL, url);

	CURLcode cr = curl_easy_perform(handle);
	long status = 0;
	long long clen = -1;
	curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_getinfo(handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &clen);
	if (enable_progress) {
		finalize_progress('DNLD');
	}
	curl_easy_cleanup(handle);

	int res = 0;
	const char* why = NULL;
	long long sz = -1;
	if (fflush(f) != 0 || ferror(f)) why = "write error (disk full?)";
	else {
		off_t o = ftello(f);
		sz = (o == (off_t)-1) ? -1 : (long long)o;
	}
	if (!why && global_quit_flag > 0) why = "cancelled";
	else if (!why && cr != CURLE_OK) why = curl_easy_strerror(cr);
	else if (!why && status != 200) why = "HTTP status is not 200";
	else if (!why && sz <= 0) why = "empty download";
	else if (!why && clen >= 0 && sz != clen) why = "size differs from Content-Length";
	else if (!why && fsync(fd) != 0) why = "fsync failed";
	if (fclose(f) != 0 && !why) why = "close failed";
	if (!why && rename(tmp, filename) != 0) why = "rename failed";
	if (why) {
		unlink(tmp);
		if (global_quit_flag > 0) res = -2;
		else {
			res = -1;
			logger(LL_ERROR, "Download of '%s' failed: %s (curl %d, status %ld, %lld bytes)\n", filename, why, (int)cr, status, sz);
		}
	}
	return res;
}
#else
int download_to_file(const char* url, const char* filename, int enable_progress)
{
	int res = 0;
	CURL* handle = curl_easy_init();
	if (handle == NULL) {
		logger(LL_ERROR, "Could not initialize CURL\n");
		return -1;
	}

	FILE* f = fopen(filename, "wb");
	if (!f) {
		logger(LL_ERROR, "Cannot open '%s' for writing\n", filename);
		return -1;
	}

	if (log_level >= LL_DEBUG)
		curl_easy_setopt(handle, CURLOPT_VERBOSE, 1);

	/* disable SSL verification to allow download from untrusted https locations */
	curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0);

	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, NULL);
	curl_easy_setopt(handle, CURLOPT_WRITEDATA, f);

	if (enable_progress > 0) {
		register_progress('DNLD', "Downloading");
#if LIBCURL_VERSION_NUM >= 0x072000
		curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, (curl_progress_callback)&download_progress);
#else
		curl_easy_setopt(handle, CURLOPT_PROGRESSFUNCTION, (curl_progress_callback)&download_progress);
#endif
	}

	curl_easy_setopt(handle, CURLOPT_NOPROGRESS, enable_progress > 0 ? 0: 1);
	curl_easy_setopt(handle, CURLOPT_USERAGENT, USER_AGENT_STRING);
	curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt(handle, CURLOPT_URL, url);

	curl_easy_perform(handle);

	if (enable_progress) {
		finalize_progress('DNLD');
	}

	curl_easy_cleanup(handle);

#ifdef WIN32
	fflush(f);
	uint64_t sz = _lseeki64(fileno(f), 0, SEEK_CUR);
#else
	off_t sz = ftello(f);
#endif
	fclose(f);

	if ((sz == 0) || ((int64_t)sz == (int64_t)-1)) {
		res = -1;
		remove(filename);
	}
	if (global_quit_flag > 0) {
		res = -2;
	}

	return res;
}
#endif
