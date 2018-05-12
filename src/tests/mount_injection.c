/* mount_injection
 *
 * Copyright © 2018 Elizaveta Tretiakova <elizabet.tretyakova@gmail.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
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


#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lxc/lxccontainer.h>
#include <lxc/list.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "lxctest.h"
#include "utils.h"

#define NAME "mount_injection_test-"
#define TEMPLATE P_tmpdir"/mount_injection_XXXXXX"

struct mountinfo_data {
	const char *mount_root;
	const char *mount_point;
	const char *fstype;
	const char *mount_source;
	const char *message;
	bool should_be_present;
};

static int comp_field(char *line, const char *str, int nfields)
{
	char *p, *p2;
	int i, ret;

	if(!line)
		return -1;

	if (!str)
		return 0;

	for (p = line, i = 0; p && i < nfields; i++)
		p = strchr(p + 1, ' ');
	if (!p)
		return -1;
	p2 = strchr(p + 1, ' ');
	if (p2)
		*p2 = '\0';
	ret = strcmp(p + 1, str);
	if (p2)
		*p2 = ' ';
	return ret;
}

static int find_in_proc_mounts(void *data)
{
	char buf[LXC_LINELEN];
	FILE *f;
	struct mountinfo_data *mdata = (struct mountinfo_data *) data;

	fprintf(stderr, "%s", mdata->message);

	f = fopen("/proc/self/mountinfo", "r");
	if (!f)
		return 0;
	while (fgets(buf, LXC_LINELEN, f)) {
		if (comp_field(buf, mdata->mount_root, 3) == 0 && comp_field(buf, mdata->mount_point, 4) == 0) {
			char *buf2 = strchr(buf, '-');
			if (comp_field(buf2, mdata->fstype, 1) == 0 && comp_field(buf2, mdata->mount_source, 2) == 0) {
				fclose(f);
				fprintf(stderr, "PRESENT\n");
				if (mdata->should_be_present)
					_exit(EXIT_SUCCESS);
				_exit(EXIT_FAILURE);
			}
		}
	}
	fclose(f);
	fprintf(stderr, "MISSING\n");
	if (!mdata->should_be_present)
		_exit(EXIT_SUCCESS);
	_exit(EXIT_FAILURE);
}

static int check_containers_mountinfo(struct lxc_container *c, struct mountinfo_data *d)
{
	pid_t pid;
	int ret = -1;
	lxc_attach_options_t attach_options = LXC_ATTACH_OPTIONS_DEFAULT;

	ret = c->attach(c, find_in_proc_mounts, d, &attach_options, &pid);
	if (ret < 0) {
		fprintf(stderr, "Check of the container's mountinfo failed\n");
		return ret;
	}

	ret = wait_for_pid(pid);
	if (ret < 0)
		fprintf(stderr, "Attached function failed");

	return ret;
}

/* config_items: NULL-terminated array of config pairs */
static int perform_container_test(const char *name, const char *config_items[])
{
	int i;
	char *sret;
	char template_log[sizeof(TEMPLATE)], template_dir[sizeof(TEMPLATE)],
			device_message[sizeof("Check urandom deivce injected into "" - ") - 1 + strlen(name) + 1],
			dir_message[sizeof("Check dir "" injected into "" - ") - 1 + sizeof(TEMPLATE) - 1 + strlen(name) + 1];
	struct lxc_container *c;
	struct lxc_mount mnt;
	struct lxc_log log;
	int ret = -1, dev_msg_size = sizeof("Check urandom deivce injected into "" - ") - 1 + strlen(name) + 1,
			dir_msg_size = sizeof("Check dir "" injected into "" - ") - 1 + sizeof(TEMPLATE) - 1 + strlen(name) + 1;
	struct mountinfo_data device = {
		.mount_root = "/",
		.mount_point = "/mnt/mount_injection_test_urandom",
		.fstype = "devtmpfs",
		.mount_source = "/dev/urandom",
		.message = "",
		.should_be_present = true
	}, dir = {
		.mount_root = template_dir,
		.mount_point = template_dir,
		.fstype = "ext4",
		.mount_source = NULL,
		.message = "",
		.should_be_present = true
	};

	/* Temp paths and messages setup */
	strcpy(template_dir, TEMPLATE);
	sret = mkdtemp(template_dir);
	if (!sret) {
		lxc_error("Failed to create temporary src file for container %s\n", name);
		exit(EXIT_FAILURE);
	}

	ret = snprintf(device_message, dev_msg_size, "Check urandom deivce injected into %s - ", name);
	if (ret < 0 || ret >= dev_msg_size) {
		fprintf(stderr, "Failed to create message for dev\n");
		exit(EXIT_FAILURE);
	}
	device.message = &device_message[0];

	ret = snprintf(dir_message, dir_msg_size, "Check dir %s injected into %s - ", template_dir, name);
	if (ret < 0 || ret >= dir_msg_size) {
		fprintf(stderr, "Failed to create message for dir\n");
		exit(EXIT_FAILURE);
	}
	dir.message = &dir_message[0];

	/* Setup logging*/
	strcpy(template_log, TEMPLATE);
	i = lxc_make_tmpfile(template_log, false);
	if (i < 0) {
		lxc_error("Failed to create temporary log file for container %s\n", name);
		exit(EXIT_FAILURE);
	} else {
		lxc_debug("Using \"%s\" as temporary log file for container %s\n", template_log, name);
		close(i);
	}

	log.name = name;
	log.file = template_log;
	log.level = "TRACE";
	log.prefix = "mount-injection";
	log.quiet = false;
	log.lxcpath = NULL;
	if (lxc_log_init(&log))
		exit(EXIT_FAILURE);

	/* Container setup */
	c = lxc_container_new(name, NULL);
	if (!c) {
		fprintf(stderr, "Unable to instantiate container (%s)...\n", name);
		goto out;
	}

	if (c->is_defined(c)) {
		fprintf(stderr, "Container (%s) already exists\n", name);
		goto out;
	}

	for (i = 0; config_items[i]; i += 2) {
		if (!c->set_config_item(c, config_items[i], config_items[i + 1])) {
			fprintf(stderr, "Failed to set \"%s\" config option to \"%s\"\n", config_items[i], config_items[i + 1]);
			goto out;
		}
	}

	if (!c->create(c, "busybox", NULL, NULL, 1, NULL)) {
		fprintf(stderr, "Creating the container (%s) failed...\n", name);
		goto out;
	}

	c->want_daemonize(c, true);

	if (!c->start(c, false, NULL)) {
		fprintf(stderr, "Starting the container (%s) failed...\n", name);
		goto out;
	}

	mnt.version = LXC_MOUNT_API_V1;

	/* Check device mounted */
	ret = c->mount(c, "/dev/urandom", "/mnt/mount_injection_test_urandom", "devtmpfs", 0, NULL, &mnt);
	if (ret < 0) {
		fprintf(stderr, "Failed to mount \"/dev/urandom\"\n");
		goto out;
	}

	ret = check_containers_mountinfo(c, &device);
	if (ret < 0)
		goto out;

	/* Check device unmounted */
	/* TODO: what about other umount flags? */
	ret = c->umount(c, "/mnt/mount_injection_test_urandom", MNT_DETACH, &mnt);
	if (ret < 0) {
		fprintf(stderr, "Failed to umount2 \"/dev/urandom\"\n");
		goto out;
	}

	device.message = "Unmounted \"/mnt/mount_injection_test_urandom\" -- should be missing now: ";
	device.should_be_present = false;
	ret = check_containers_mountinfo(c, &device);
	if (ret < 0)
		goto out;

	/* Check dir mounted */
	ret = c->mount(c, template_dir, template_dir, "ext4", MS_BIND, NULL, &mnt);
	if (ret < 0) {
		fprintf(stderr, "Failed to mount \"%s\"\n", template_dir);
		goto out;
	}

	ret = check_containers_mountinfo(c, &dir);
	if (ret < 0)
		goto out;

	/* Check dir unmounted */
	/* TODO: what about other umount flags? */
	ret = c->umount(c, template_dir, MNT_DETACH, &mnt);
	if (ret < 0) {
		fprintf(stderr, "Failed to umount2 \"%s\"\n", template_dir);
		goto out;
	}

	dir.message = "Unmounted dir -- should be missing now: ";
	dir.should_be_present = false;
	ret = check_containers_mountinfo(c, &dir);
	if (ret < 0)
		goto out;

	/* Finalize the container */
	if (!c->stop(c)) {
		fprintf(stderr, "Stopping the container (%s) failed...\n", name);
		goto out;
	}

	if (!c->destroy(c)) {
		fprintf(stderr, "Destroying the container (%s) failed...\n", name);
		goto out;
	}

	ret = 0;
out:
	lxc_container_put(c);

	if (ret != 0) {
		int fd;

		fd = open(template_log, O_RDONLY);
		if (fd >= 0) {
			char buf[4096];
			ssize_t buflen;
			while ((buflen = read(fd, buf, 1024)) > 0) {
				buflen = write(STDERR_FILENO, buf, buflen);
				if (buflen <= 0)
					break;
			}
			close(fd);
		}
	}

	unlink(template_log);
	unlink(template_dir);

	return ret;
}

static int do_priv_container_test()
{
	const char *config_items[] = {"lxc.mount.auto", "shmounts:/tmp/mount_injection_test", NULL};
	return perform_container_test(NAME"privileged", config_items);
}

static int do_unpriv_container_test()
{
	const char *config_items[] = {
		"lxc.mount.auto", "shmounts:/tmp/mount_injection_test",
		"lxc.init.uid", "100000",
		"lxc.init.gid", "100000",
		NULL
	};
	return perform_container_test(NAME"unprivileged", config_items);
}

int main(int argc, char *argv[])
{
	if (do_priv_container_test()) {
		fprintf(stderr, "Privileged mount injection test failed\n");
		return -1;
	}
	if(do_unpriv_container_test()) {
		fprintf(stderr, "Unprivileged mount injection test failed\n");
		return -1;
	}
	return 0;
}
