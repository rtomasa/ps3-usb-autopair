/*
 * ps3-usb-autopair — DualShock 3 / Sixaxis USB auto-pair helper.
 *
 * Uses kernel hidraw ioctls (same protocol as BlueZ sixaxis plugin) and writes
 * BlueZ cache/info files so the controller can reconnect over Bluetooth.
 *
 * Modes:
 *   ps3-usb-autopair --pair DEV   — pair one hidraw node (started by udev/systemd)
 *   ps3-usb-autopair --status     — print current status JSON to stdout
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sdp_ds3.h"

#define STATUS_DIR "/run/ps3-usb-autopair"
#define STATUS_FILE STATUS_DIR "/status"
#define PAIR_LOCK STATUS_DIR "/pair.lock"
#define BT_VAR_LIB "/var/lib/bluetooth"
#define HID_UUID "00001124-0000-1000-8000-00805f9b34fb"

static int write_file_atomic(const char *path, const char *data, mode_t mode);

static void ensure_status_dir(void)
{
	mkdir(STATUS_DIR, 0755);
}

static void json_escape(const char *in, char *out, size_t outsz)
{
	size_t j = 0;
	size_t i;

	for (i = 0; in[i] && j + 2 < outsz; i++) {
		if (in[i] == '"' || in[i] == '\\') {
			out[j++] = '\\';
			out[j++] = in[i];
		} else if ((unsigned char)in[i] < 0x20) {
			out[j++] = ' ';
		} else {
			out[j++] = in[i];
		}
	}
	out[j] = '\0';
}

static void write_status(const char *state, const char *detail, ...)
{
	char msg[512];
	char esc[640];
	char json[896];
	va_list ap;
	time_t now = time(NULL);

	va_start(ap, detail);
	vsnprintf(msg, sizeof(msg), detail ? detail : "", ap);
	va_end(ap);
	json_escape(msg, esc, sizeof(esc));

	ensure_status_dir();
	snprintf(json, sizeof(json),
		"{\n"
		"  \"state\": \"%s\",\n"
		"  \"detail\": \"%s\",\n"
		"  \"reboot_required\": false,\n"
		"  \"updated\": %ld\n"
		"}\n",
		state, esc, (long)now);
	(void)write_file_atomic(STATUS_FILE, json, 0644);
}

static void mac_bytes_to_str(const uint8_t mac[6], char out[18])
{
	snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
		 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool mac_is_zero(const uint8_t mac[6])
{
	static const uint8_t zero[6];

	return memcmp(mac, zero, sizeof(zero)) == 0;
}

static int parse_mac(const char *s, uint8_t mac[6])
{
	unsigned int b[6];
	char trailing;
	int i;

	if (strlen(s) != 17)
		return -1;
	if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c",
		   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &trailing) != 6)
		return -1;
	for (i = 0; i < 6; i++) {
		if (b[i] > UINT8_MAX)
			return -1;
		mac[i] = (uint8_t)b[i];
	}
	return 0;
}

static int read_adapter_mac(uint8_t mac[6])
{
	DIR *d;
	struct dirent *de;
	const char *preferred = "/sys/class/bluetooth/hci0/address";
	char line[128];
	FILE *f;

	/* Prefer hci0 so multiple adapters never depend on readdir order. */
	f = fopen(preferred, "r");
	if (f) {
		if (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = '\0';
			if (parse_mac(line, mac) == 0 && !mac_is_zero(mac)) {
				fclose(f);
				return 0;
			}
		}
		fclose(f);
	}

	/* Other adapters, for systems without hci0. */
	d = opendir("/sys/class/bluetooth");
	if (d) {
		while ((de = readdir(d)) != NULL) {
			char path[128];
			if (strncmp(de->d_name, "hci", 3) != 0 ||
			    strcmp(de->d_name, "hci0") == 0)
				continue;
			if (snprintf(path, sizeof(path),
				     "/sys/class/bluetooth/%s/address",
				     de->d_name) >= (int)sizeof(path))
				continue;
			f = fopen(path, "r");
			if (!f)
				continue;
			if (!fgets(line, sizeof(line), f)) {
				fclose(f);
				continue;
			}
			fclose(f);
			line[strcspn(line, "\r\n")] = '\0';
			if (parse_mac(line, mac) == 0 && !mac_is_zero(mac)) {
				closedir(d);
				return 0;
			}
		}
		closedir(d);
	}

	/* Legacy fallback for kernels that do not expose the address in sysfs. */
	f = popen("hciconfig 2>/dev/null", "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			const char *p = strstr(line, "BD Address:");
			char address[18];
			if (!p)
				continue;
			p += strlen("BD Address:");
			while (*p == ' ' || *p == '\t')
				p++;
			if (strlen(p) < 17)
				continue;
			memcpy(address, p, 17);
			address[17] = '\0';
			if (parse_mac(address, mac) == 0 && !mac_is_zero(mac)) {
				pclose(f);
				return 0;
			}
		}
		pclose(f);
	}

	return -1;
}

static int sixaxis_get_device_bdaddr(int fd, uint8_t mac[6])
{
	uint8_t buf[18];

	memset(buf, 0, sizeof(buf));
	buf[0] = 0xf2;
	if (ioctl(fd, HIDIOCGFEATURE(sizeof(buf)), buf) < 0)
		return -1;
	/* The report contains the address in normal, printable byte order. */
	memcpy(mac, buf + 4, 6);
	return 0;
}

static int sixaxis_get_central_bdaddr(int fd, uint8_t mac[6])
{
	uint8_t buf[8];

	memset(buf, 0, sizeof(buf));
	buf[0] = 0xf5;
	if (ioctl(fd, HIDIOCGFEATURE(sizeof(buf)), buf) < 0)
		return -1;
	memcpy(mac, buf + 2, 6);
	return 0;
}

static int sixaxis_set_central_bdaddr(int fd, const uint8_t mac[6])
{
	uint8_t buf[8];

	/* hidraw: report 0xF5, then 01 00 is implied by device; write 01 + MAC
	 * as F5 01 m0..m5 — device stores/returns 01 00 m0..m5. */
	buf[0] = 0xf5;
	buf[1] = 0x01;
	memcpy(buf + 2, mac, 6);
	if (ioctl(fd, HIDIOCSFEATURE(sizeof(buf)), buf) < 0)
		return -1;
	return 0;
}

static int mkdir_p(const char *path, mode_t mode)
{
	char tmp[512];
	size_t len;
	size_t i;

	if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	len = strlen(tmp);
	if (len == 0)
		return -1;
	if (tmp[len - 1] == '/')
		tmp[len - 1] = '\0';

	for (i = 1; tmp[i]; i++) {
		if (tmp[i] != '/')
			continue;
		tmp[i] = '\0';
		if (mkdir(tmp, mode) < 0 && errno != EEXIST)
			return -1;
		tmp[i] = '/';
	}
	if (mkdir(tmp, mode) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

static int write_file_atomic(const char *path, const char *data, mode_t mode)
{
	char tmp[192];
	size_t left = strlen(data);
	const char *p = data;
	int fd;
	int saved_errno = 0;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path) >=
	    (int)sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	fd = mkstemp(tmp);
	if (fd < 0)
		return -1;
	if (fchmod(fd, mode) < 0)
		goto fail;
	while (left > 0) {
		ssize_t written = write(fd, p, left);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			goto fail;
		}
		if (written == 0) {
			errno = EIO;
			goto fail;
		}
		p += written;
		left -= (size_t)written;
	}
	if (fsync(fd) < 0)
		goto fail;
	if (close(fd) < 0) {
		fd = -1;
		goto fail;
	}
	fd = -1;
	if (rename(tmp, path) < 0)
		goto fail;
	return 0;

fail:
	saved_errno = errno;
	if (fd >= 0)
		close(fd);
	unlink(tmp);
	errno = saved_errno;
	return -1;
}

static int write_bluez_trust_and_sdp(const uint8_t adapter[6],
				    const uint8_t device[6])
{
	char adapter_s[18], device_s[18];
	char dir[128], info_path[160], cache_path[160], cache_dir[128];
	char info[1024];
	char cache[2048];
	int len;

	mac_bytes_to_str(adapter, adapter_s);
	mac_bytes_to_str(device, device_s);

	snprintf(dir, sizeof(dir), "%s/%s/%s", BT_VAR_LIB, adapter_s, device_s);
	snprintf(info_path, sizeof(info_path), "%s/info", dir);
	snprintf(cache_dir, sizeof(cache_dir), "%s/%s/cache", BT_VAR_LIB,
		 adapter_s);
	snprintf(cache_path, sizeof(cache_path), "%s/%s", cache_dir, device_s);

	if (mkdir_p(dir, 0700) < 0)
		return -1;
	if (mkdir_p(cache_dir, 0700) < 0)
		return -1;

	len = snprintf(info, sizeof(info),
		"[General]\n"
		"Name=Sony PLAYSTATION(R)3 Controller\n"
		"Class=0x000508\n"
		"SupportedTechnologies=BR/EDR;\n"
		"Trusted=true\n"
		"Blocked=false\n"
		"CablePairing=true\n"
		"Services=%s;\n"
		"\n"
		"[DeviceID]\n"
		"Source=2\n"
		"Vendor=1356\n"
		"Product=616\n"
		"Version=0\n",
		HID_UUID);
	if (len < 0 || len >= (int)sizeof(info)) {
		errno = EOVERFLOW;
		return -1;
	}
	if (write_file_atomic(info_path, info, 0600) < 0)
		return -1;

	len = snprintf(cache, sizeof(cache),
		"[General]\n"
		"Name=Sony PLAYSTATION(R)3 Controller\n"
		"\n"
		"[ServiceRecords]\n"
		"0x00010000=%s\n",
		SIXAXIS_HID_SDP_RECORD);
	if (len < 0 || len >= (int)sizeof(cache)) {
		errno = EOVERFLOW;
		return -1;
	}
	if (write_file_atomic(cache_path, cache, 0600) < 0)
		return -1;

	return 0;
}

static int read_hidraw_uniq(const char *devnode, uint8_t mac[6])
{
	const char *base;
	char path[160];
	char line[256];
	FILE *f;

	base = strrchr(devnode, '/');
	base = base ? base + 1 : devnode;

	/* Prefer uevent (HID_UNIQ=...) — uniq sysfs node is absent on some kernels. */
	if (snprintf(path, sizeof(path),
		     "/sys/class/hidraw/%s/device/uevent",
		     base) < (int)sizeof(path)) {
		f = fopen(path, "r");
		if (f) {
			while (fgets(line, sizeof(line), f)) {
				if (strncmp(line, "HID_UNIQ=", 9) != 0)
					continue;
				line[strcspn(line, "\r\n")] = '\0';
				fclose(f);
				return parse_mac(line + 9, mac);
			}
			fclose(f);
		}
	}

	if (snprintf(path, sizeof(path),
		     "/sys/class/hidraw/%s/device/uniq",
		     base) >= (int)sizeof(path))
		return -1;
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	line[strcspn(line, "\r\n")] = '\0';
	return parse_mac(line, mac);
}

static int systemctl_bluetooth(const char *action)
{
	pid_t pid = fork();
	pid_t waited;
	int status;

	if (pid < 0)
		return -1;
	if (pid == 0) {
		execlp("systemctl", "systemctl", action, "bluetooth.service",
		       (char *)NULL);
		_exit(127);
	}
	do {
		waited = waitpid(pid, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited < 0)
		return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static int pair_hidraw_locked(const char *devnode)
{
	int fd;
	uint8_t adapter[6], device[6], central[6];
	char adapter_s[18], device_s[18], central_s[18];
	struct timespec retry = { .tv_sec = 0, .tv_nsec = 250000000 };
	int attempt;
	int store_result;
	int store_errno;

	write_status("pairing", "USB controller detected, pairing...");

	for (attempt = 0; attempt < 40; attempt++) {
		if (read_adapter_mac(adapter) == 0)
			break;
		nanosleep(&retry, NULL);
	}
	if (attempt == 40) {
		write_status("error", "No Bluetooth adapter (hci) found");
		return -1;
	}
	mac_bytes_to_str(adapter, adapter_s);

	fd = open(devnode, O_RDWR);
	if (fd < 0) {
		write_status("error", "Cannot open %s: %s", devnode,
			     strerror(errno));
		return -1;
	}

	/*
	 * Prefer HID_UNIQ from sysfs for the identity exposed by the kernel.
	 * Feature report 0xF2 is the fallback for kernels without that entry.
	 */
	if (read_hidraw_uniq(devnode, device) < 0) {
		if (sixaxis_get_device_bdaddr(fd, device) < 0) {
			write_status("error", "Failed to read controller MAC");
			close(fd);
			return -1;
		}
	}
	if (mac_is_zero(device)) {
		write_status("error", "Controller returned an invalid MAC address");
		close(fd);
		return -1;
	}
	mac_bytes_to_str(device, device_s);

	if (sixaxis_get_central_bdaddr(fd, central) < 0) {
		write_status("error", "Failed to read current master MAC");
		close(fd);
		return -1;
	}
	mac_bytes_to_str(central, central_s);

	if (memcmp(adapter, central, 6) != 0) {
		if (sixaxis_set_central_bdaddr(fd, adapter) < 0) {
			write_status("error", "Failed to write master MAC");
			close(fd);
			return -1;
		}
		if (sixaxis_get_central_bdaddr(fd, central) < 0 ||
		    memcmp(adapter, central, 6) != 0) {
			write_status("error", "Master MAC verification failed");
			close(fd);
			return -1;
		}
	}
	close(fd);

	/* BlueZ owns its private store. Stop it so it cannot overwrite our
	 * cable-pairing record during shutdown, then start it to load the files. */
	if (systemctl_bluetooth("stop") < 0) {
		write_status("error", "Failed to stop Bluetooth before updating its store");
		return -1;
	}
	store_result = write_bluez_trust_and_sdp(adapter, device);
	store_errno = errno;
	if (systemctl_bluetooth("start") < 0) {
		write_status("error", "Failed to restart Bluetooth after pairing");
		return -1;
	}
	if (store_result < 0) {
		errno = store_errno;
		write_status("error", "Failed to write BlueZ trust/SDP: %s",
			     strerror(errno));
		return -1;
	}

	write_status("waiting_ps",
		     "Paired %s -> adapter %s (was %s). Unplug USB and press PS.",
		     device_s, adapter_s, central_s);
	return 0;
}

static int pair_hidraw(const char *devnode)
{
	int lock_fd;
	int result;

	ensure_status_dir();
	lock_fd = open(PAIR_LOCK, O_CREAT | O_CLOEXEC | O_RDWR, 0600);
	if (lock_fd < 0) {
		write_status("error", "Cannot create pairing lock: %s",
			     strerror(errno));
		return -1;
	}
	if (flock(lock_fd, LOCK_EX) < 0) {
		write_status("error", "Cannot acquire pairing lock: %s",
			     strerror(errno));
		close(lock_fd);
		return -1;
	}
	result = pair_hidraw_locked(devnode);
	close(lock_fd);
	return result;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s --pair DEV      Pair hidraw device (udev)\n"
		"  %s --status        Print status file\n",
		argv0, argv0);
}

int main(int argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "--pair") == 0) {
		const char *dev;
		if (argc != 3) {
			usage(argv[0]);
			return 1;
		}
		dev = argv[2];
		/* udev may pass "hidraw0" without /dev/ */
		if (strncmp(dev, "/dev/", 5) != 0) {
			char path[64];
			if (snprintf(path, sizeof(path), "/dev/%s", dev) >=
			    (int)sizeof(path)) {
				fprintf(stderr, "Device name is too long\n");
				return 1;
			}
			return pair_hidraw(path) == 0 ? 0 : 1;
		}
		return pair_hidraw(dev) == 0 ? 0 : 1;
	}

	if (argc == 2 && strcmp(argv[1], "--status") == 0) {
		FILE *f = fopen(STATUS_FILE, "r");
		char buf[512];
		if (!f) {
			puts("{\"state\":\"unknown\",\"detail\":\"no status yet\"}");
			return 0;
		}
		while (fgets(buf, sizeof(buf), f))
			fputs(buf, stdout);
		fclose(f);
		return 0;
	}

	usage(argv[0]);
	return 1;
}
