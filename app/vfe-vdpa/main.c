/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2018 Intel Corporation
 */

#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <uuid/uuid.h>

#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_vhost.h>
#include <rte_vdpa.h>
#include <rte_pci.h>
#include <rte_uuid.h>
#include <rte_string_fns.h>
#include <virtio_api.h>
#include <virtio_lm.h>
#include <rte_vf_rpc.h>
#include <rte_vfio.h>

#include <cmdline_parse.h>
#include <cmdline_socket.h>
#include <cmdline_parse_string.h>
#include <cmdline_parse_num.h>
#include <cmdline.h>

#include "vdpa_rpc.h"

static struct vdpa_rpc_context vdpa_rpc_ctx;

#define RTE_LOGTYPE_VDPA RTE_LOGTYPE_USER1

struct vdpa_port {
	char ifname[MAX_PATH_LEN];
	struct rte_vdpa_device *dev;
	int vid;
	uint64_t flags;
	int stats_n;
	struct rte_vdpa_stat_name *stats_names;
	struct rte_vdpa_stat *stats;
};

static struct vdpa_port vports[MAX_VDPA_SAMPLE_PORTS];

static char iface[MAX_IF_PATH_LEN];
static int devcnt;
static int interactive;
static int client_mode;

/* display usage */
static void
vdpa_usage(const char *prgname)
{
	printf("Usage: %s [EAL options] -- "
				 "	--interactive|-i: run in interactive mode.\n"
				 "	--iface <path>: specify the path prefix of the socket files, e.g. /tmp/vhost-user-.\n"
				 "	--client: register a vhost-user socket as client mode.\n",
				 prgname);
}

static int
parse_args(int argc, char **argv)
{
	static const char *short_option = "i";
	static struct option long_option[] = {
		{"iface", required_argument, NULL, 0},
		{"interactive", no_argument, &interactive, 1},
		{"client", no_argument, &client_mode, 1},
		{NULL, 0, 0, 0},
	};
	int opt, idx;
	char *prgname = argv[0];

	while ((opt = getopt_long(argc, argv, short_option, long_option, &idx))
			!= EOF) {
		switch (opt) {
		case 'i':
			printf("Interactive-mode selected\n");
			interactive = 1;
			break;
		/* long options */
		case 0:
			if (strncmp(long_option[idx].name, "iface",
						MAX_IF_PATH_LEN) == 0) {
				if (rte_strscpy(iface, optarg, MAX_IF_PATH_LEN) < 0) {
					printf("Invalid iface length \n");
					return -1;
				}
				printf("iface %s\n", iface);
			}
			if (!strcmp(long_option[idx].name, "interactive")) {
				printf("Interactive-mode selected\n");
				interactive = 1;
			}
			break;

		default:
			vdpa_usage(prgname);
			return -1;
		}
	}

	return 0;
}

static int
new_device(int vid)
{
	char ifname[MAX_PATH_LEN];
	struct rte_device *dev;
	int i;

	rte_vhost_get_ifname(vid, ifname, sizeof(ifname));
	for (i = 0; i < MAX_VDPA_SAMPLE_PORTS; i++) {
		if (strncmp(ifname, vports[i].ifname, MAX_PATH_LEN))
			continue;

		dev = rte_vdpa_get_rte_device(vports[i].dev);
		if (!dev) {
			RTE_LOG(ERR, VDPA,
				"Failed to get generic device for port %d\n", i);
			continue;
		}
		printf("\nnew port %s, device : %s\n", ifname, dev->name);
		vports[i].vid = vid;
		break;
	}

	if (i >= MAX_VDPA_SAMPLE_PORTS)
		return -1;

	return 0;
}

static void
destroy_device(int vid)
{
	struct rte_device *dev;
	char ifname[MAX_PATH_LEN];
	int i;

	rte_vhost_get_ifname(vid, ifname, sizeof(ifname));
	for (i = 0; i < MAX_VDPA_SAMPLE_PORTS; i++) {
		if (strncmp(ifname, vports[i].ifname, MAX_PATH_LEN))
			continue;

		dev = rte_vdpa_get_rte_device(vports[i].dev);
		if (!dev) {
			RTE_LOG(ERR, VDPA,
				"Failed to get generic device for port %d\n", i);
			continue;
		}

		printf("\ndestroy port %s, device: %s\n", ifname, dev->name);
		break;
	}
}

static const struct rte_vhost_device_ops vdpa_sample_devops = {
	.new_device = new_device,
	.destroy_device = destroy_device,
};

static int
start_vdpa(struct vdpa_port *vport)
{
	int ret;
	char *socket_path = vport->ifname;
	uint64_t features;

	if (client_mode)
		vport->flags |= RTE_VHOST_USER_CLIENT;

	if (access(socket_path, F_OK) != -1 && !client_mode) {
		RTE_LOG(ERR, VDPA,
			"%s exists, please remove it or specify another file and try again.\n",
			socket_path);
		return -1;
	}
	ret = rte_vhost_driver_register(socket_path, vport->flags);
	if (ret != 0)
		rte_exit(EXIT_FAILURE,
			"register driver failed: %s\n",
			socket_path);

	/* vdpa device should get feature from device and not use builtin_net_driver */
	ret = rte_vdpa_get_features(vport->dev, &features);
	if (ret != 0)
		rte_exit(EXIT_FAILURE,
			"get vdpa feature fail: %s\n",
			socket_path);

	ret = rte_vhost_driver_set_features(socket_path, features);
	if (ret != 0)
		rte_exit(EXIT_FAILURE,
			"set feature failed: %s\n",
			socket_path);

	ret = rte_vhost_driver_callback_register(socket_path,
			&vdpa_sample_devops);
	if (ret != 0)
		rte_exit(EXIT_FAILURE,
			"register driver ops failed: %s\n",
			socket_path);

	ret = rte_vhost_driver_attach_vdpa_device(socket_path, vport->dev);
	if (ret != 0)
		rte_exit(EXIT_FAILURE,
			"attach vdpa device failed: %s\n",
			socket_path);

	if (rte_vhost_driver_start(socket_path) < 0)
		rte_exit(EXIT_FAILURE,
			"start vhost driver failed: %s\n",
			socket_path);
	return 0;
}

static void
close_vdpa(struct vdpa_port *vport)
{
	int ret;
	char *socket_path = vport->ifname;

	ret = rte_vhost_driver_detach_vdpa_device(socket_path);
	if (ret != 0)
		RTE_LOG(ERR, VDPA,
				"detach vdpa device failed: %s\n",
				socket_path);

	ret = rte_vhost_driver_unregister(socket_path);
	if (ret != 0)
		RTE_LOG(ERR, VDPA,
				"Fail to unregister vhost driver for %s.\n",
				socket_path);
	if (vport->stats_names) {
		rte_free(vport->stats_names);
		vport->stats_names = NULL;
	}
}

int vdpa_with_socket_path_start(const char *vf_name,
		const char *socket_file)
{
	struct rte_vdpa_device *dev;
	char ifname[MAX_PATH_LEN];

	if (iface[0] == '\0' && !socket_file) {
		/* Default socket path: /tmp/vf- */
		snprintf(ifname, MAX_PATH_LEN,
		"/tmp/vf-%s", vf_name);
	} else {
		if (socket_file)
			snprintf(ifname, MAX_PATH_LEN,
				"%s", socket_file);
		else
			snprintf(ifname, MAX_PATH_LEN,
				"%s%s", iface, vf_name);
	}
	rte_strscpy(vports[devcnt].ifname, ifname, MAX_PATH_LEN);
	RTE_LOG(INFO, VDPA, "VDPA VF Device %s socket file is %s\n",
		vf_name, ifname);
	dev = rte_vdpa_find_device_by_name(vf_name);
	if (dev == NULL) {
		RTE_LOG(ERR, VDPA, "Unable to find vdpa device id for %s.\n",
		vf_name);
		return -1;
	}
	vports[devcnt].dev = dev;
	if (start_vdpa(&vports[devcnt]) == 0)
		devcnt++;
	return 0;
}

void vdpa_with_socket_path_stop(const char *vf_name)
{
	struct rte_vdpa_device *dev;
	struct vdpa_port *vport = NULL;
	int i;
	dev = rte_vdpa_find_device_by_name(vf_name);
	if (dev == NULL) {
		RTE_LOG(ERR, VDPA, "Unable to find vdpa device id for %s.\n",
		vf_name);
		return;
	}
	for (i = 0; i < RTE_MIN(MAX_VDPA_SAMPLE_PORTS, devcnt); i++) {
		if (vports[i].dev == dev) {
			vport = &vports[i];
			break;
		}
	}
	if (vport == NULL) {
		RTE_LOG(ERR, VDPA, "Unable to find vdpa device port for %s.\n",
		vf_name);
		return;
	}
	if (vport->ifname[0] != '\0')
		close_vdpa(vport);
}

int vdpa_get_socket_file_name(const char *vf_name,
		char *socket_file)
{
	struct rte_vdpa_device *dev;
	struct vdpa_port *vport = NULL;
	int i;
	dev = rte_vdpa_find_device_by_name(vf_name);
	if (dev == NULL) {
		RTE_LOG(ERR, VDPA, "Unable to find vdpa device id for %s.\n",
		vf_name);
		return -1;
	}
	for (i = 0; i < RTE_MIN(MAX_VDPA_SAMPLE_PORTS, devcnt); i++) {
		if (vports[i].dev == dev) {
			vport = &vports[i];
			break;
		}
	}
	if (vport == NULL) {
		RTE_LOG(ERR, VDPA, "Unable to find vdpa device port for %s.\n",
		vf_name);
		return -1;
	}
	if (rte_strscpy(socket_file, vport->ifname, MAX_PATH_LEN) < 0) {
		RTE_LOG(ERR, VDPA, "Unable to dump socket file for %s.\n",
		vf_name);
		return -1;
	}
	return 0;
}

static void
vdpa_sample_quit(void)
{
	int i;
	for (i = 0; i < RTE_MIN(MAX_VDPA_SAMPLE_PORTS, devcnt); i++) {
		if (vports[i].ifname[0] != '\0')
			close_vdpa(&vports[i]);
	}
}
#define RTE_DEV_FOREACH_SAFE(dev, devstr, it, tdev) \
    for (rte_dev_iterator_init(it, devstr), \
        (dev) = rte_dev_iterator_next(it); \
        (dev) && ((tdev) = rte_dev_iterator_next(it), 1); \
        (dev) = (tdev))
bool rpc_start=false;
bool eal_start=false;

static void
signal_handler(int signum)
{
	struct rte_device *dev;
	struct rte_dev_iterator dev_iter;
	struct rte_device *tdev = NULL;

	if (signum == SIGINT || signum == SIGTERM) {
		printf("\nSignal %d received, preparing to exit...\n", signum);
		if (rpc_start)
			vdpa_rpc_stop(&vdpa_rpc_ctx);

		if (eal_start) {
			vdpa_sample_quit();

			RTE_DEV_FOREACH_SAFE(dev, "class=vdpa", &dev_iter, tdev) {
				rte_dev_remove(dev);
			}

			RTE_DEV_FOREACH_SAFE(dev, "bus=pci", &dev_iter, tdev) {
				rte_dev_remove(dev);
			}

			printf("\n Remove default container fd \r\n");
			if (rte_vfio_container_destroy(RTE_VFIO_DEFAULT_CONTAINER_FD) != 0) {
				printf("\n Default container fd remove fail \r\n");
			}
			/* clean up the EAL */
			rte_eal_cleanup();
		}
		exit(0);
	}
}
static void *
sig_thread(void*arg)
{
	sigset_t *set=(sigset_t*) arg;
	int s, sig;

	for (;;){
		s = sigwait(set,&sig);
		if (s != 0)
			printf("\n signal wait error:%d \r\n", s);
		signal_handler(sig);
	}

	return NULL;
}

/* interactive cmds */

/* *** Help command with introduction. *** */
struct cmd_help_result {
	cmdline_fixed_string_t help;
};

static void cmd_help_parsed(__rte_unused void *parsed_result,
		struct cmdline *cl,
		__rte_unused void *data)
{
	cmdline_printf(
		cl,
		"\n"
		"The following commands are currently available:\n\n"
		"Control:\n"
		"    help                                      : Show interactive instructions.\n"
		"    list                                      : list all available vdpa devices.\n"
		"    create <socket file> <vdev addr>          : create a new vdpa port.\n"
		"    stats <device ID> <virtio queue ID>       : show statistics of virtio queue, 0xffff for all.\n"
		"    quit                                      : exit vdpa sample app.\n"
	);
}

cmdline_parse_token_string_t cmd_help_help =
	TOKEN_STRING_INITIALIZER(struct cmd_help_result, help, "help");

cmdline_parse_inst_t cmd_help = {
	.f = cmd_help_parsed,
	.data = NULL,
	.help_str = "show help",
	.tokens = {
		(void *)&cmd_help_help,
		NULL,
	},
};

/* *** List all available vdpa devices *** */
struct cmd_list_result {
	cmdline_fixed_string_t action;
};

static void cmd_list_vdpa_devices_parsed(
		__rte_unused void *parsed_result,
		struct cmdline *cl,
		__rte_unused void *data)
{
	uint32_t queue_num;
	uint64_t features;
	struct rte_vdpa_device *vdev;
	struct rte_device *dev;
	struct rte_dev_iterator dev_iter;

	cmdline_printf(cl, "device name\tqueue num\tsupported features\n");
	RTE_DEV_FOREACH(dev, "class=vdpa", &dev_iter) {
		vdev = rte_vdpa_find_device_by_name(dev->name);
		if (!vdev)
			continue;
		if (rte_vdpa_get_queue_num(vdev, &queue_num) < 0) {
			RTE_LOG(ERR, VDPA,
				"failed to get vdpa queue number "
				"for device %s.\n", dev->name);
			continue;
		}
		if (rte_vdpa_get_features(vdev, &features) < 0) {
			RTE_LOG(ERR, VDPA,
				"failed to get vdpa features "
				"for device %s.\n", dev->name);
			continue;
		}
		cmdline_printf(cl, "%s\t\t%" PRIu32 "\t\t0x%" PRIx64 "\n",
			dev->name, queue_num, features);
	}
}

cmdline_parse_token_string_t cmd_action_list =
	TOKEN_STRING_INITIALIZER(struct cmd_list_result, action, "list");

cmdline_parse_inst_t cmd_list_vdpa_devices = {
	.f = cmd_list_vdpa_devices_parsed,
	.data = NULL,
	.help_str = "list all available vdpa devices",
	.tokens = {
		(void *)&cmd_action_list,
		NULL,
	},
};

/* *** Create new vdpa port *** */
struct cmd_create_result {
	cmdline_fixed_string_t action;
	cmdline_fixed_string_t socket_path;
	cmdline_fixed_string_t bdf;
};

static void cmd_create_vdpa_port_parsed(void *parsed_result,
		struct cmdline *cl,
		__rte_unused void *data)
{
	struct rte_vdpa_device *dev;
	struct cmd_create_result *res = parsed_result;

	rte_strscpy(vports[devcnt].ifname, res->socket_path, MAX_PATH_LEN);
	dev = rte_vdpa_find_device_by_name(res->bdf);
	if (dev == NULL) {
		cmdline_printf(cl, "Unable to find vdpa device id for %s.\n",
				res->bdf);
		return;
	}

	vports[devcnt].dev = dev;

	if (start_vdpa(&vports[devcnt]) == 0)
		devcnt++;
}

cmdline_parse_token_string_t cmd_action_create =
	TOKEN_STRING_INITIALIZER(struct cmd_create_result, action, "create");
cmdline_parse_token_string_t cmd_socket_path =
	TOKEN_STRING_INITIALIZER(struct cmd_create_result, socket_path, NULL);
cmdline_parse_token_string_t cmd_bdf =
	TOKEN_STRING_INITIALIZER(struct cmd_create_result, bdf, NULL);

cmdline_parse_inst_t cmd_create_vdpa_port = {
	.f = cmd_create_vdpa_port_parsed,
	.data = NULL,
	.help_str = "create a new vdpa port",
	.tokens = {
		(void *)&cmd_action_create,
		(void *)&cmd_socket_path,
		(void *)&cmd_bdf,
		NULL,
	},
};

/* *** STATS *** */
struct cmd_stats_result {
	cmdline_fixed_string_t stats;
	cmdline_fixed_string_t bdf;
	uint16_t qid;
};

static void cmd_device_stats_parsed(void *parsed_result, struct cmdline *cl,
				    __rte_unused void *data)
{
	struct cmd_stats_result *res = parsed_result;
	struct rte_vdpa_device *vdev = rte_vdpa_find_device_by_name(res->bdf);
	struct vdpa_port *vport = NULL;
	uint32_t first, last;
	int i;

	if (!vdev) {
		RTE_LOG(ERR, VDPA, "Invalid device: %s.\n",
			res->bdf);
		return;
	}
	for (i = 0; i < RTE_MIN(MAX_VDPA_SAMPLE_PORTS, devcnt); i++) {
		if (vports[i].dev == vdev) {
			vport = &vports[i];
			break;
		}
	}
	if (!vport) {
		RTE_LOG(ERR, VDPA, "Device %s was not created.\n", res->bdf);
		return;
	}
	if (res->qid == 0xFFFF) {
		first = 0;
		last = rte_vhost_get_vring_num(vport->vid);
		if (last == 0) {
			RTE_LOG(ERR, VDPA, "Failed to get num of actual virtqs"
				" for device %s.\n", res->bdf);
			return;
		}
		last--;
	} else {
		first = res->qid;
		last = res->qid;
	}
	if (!vport->stats_names) {
		vport->stats_n = rte_vdpa_get_stats_names(vport->dev, NULL, 0);
		if (vport->stats_n <= 0) {
			RTE_LOG(ERR, VDPA, "Failed to get names number of "
				"device %s stats.\n", res->bdf);
			return;
		}
		vport->stats_names = rte_zmalloc(NULL,
			(sizeof(*vport->stats_names) + sizeof(*vport->stats)) *
							vport->stats_n, 0);
		if (!vport->stats_names) {
			RTE_LOG(ERR, VDPA, "Failed to allocate memory for stat"
				" names of device %s.\n", res->bdf);
			return;
		}
		i = rte_vdpa_get_stats_names(vport->dev, vport->stats_names,
						vport->stats_n);
		if (vport->stats_n != i) {
			RTE_LOG(ERR, VDPA, "Failed to get names of device %s "
				"stats.\n", res->bdf);
			return;
		}
		vport->stats = (struct rte_vdpa_stat *)
					(vport->stats_names + vport->stats_n);
	}
	cmdline_printf(cl, "\nDevice %s:\n", res->bdf);
	for (; first <= last; first++) {
		memset(vport->stats, 0, sizeof(*vport->stats) * vport->stats_n);
		if (rte_vdpa_get_stats(vport->dev, (int)first, vport->stats,
					vport->stats_n) <= 0) {
			RTE_LOG(ERR, VDPA, "Failed to get vdpa queue statistics"
				" for device %s qid %d.\n", res->bdf,
				(int)first);
			return;
		}
		cmdline_printf(cl, "\tVirtq %" PRIu32 ":\n", first);
		for (i = 0; i < vport->stats_n; ++i) {
			cmdline_printf(cl, "\t\t%-*s %-16" PRIu64 "\n",
				RTE_VDPA_STATS_NAME_SIZE,
				vport->stats_names[vport->stats[i].id].name,
				vport->stats[i].value);
		}
	}
}

cmdline_parse_token_string_t cmd_device_stats_ =
	TOKEN_STRING_INITIALIZER(struct cmd_stats_result, stats, "stats");
cmdline_parse_token_string_t cmd_device_bdf =
	TOKEN_STRING_INITIALIZER(struct cmd_stats_result, bdf, NULL);
cmdline_parse_token_num_t cmd_queue_id =
	TOKEN_NUM_INITIALIZER(struct cmd_stats_result, qid, RTE_UINT32);

cmdline_parse_inst_t cmd_device_stats = {
	.f = cmd_device_stats_parsed,
	.data = NULL,
	.help_str = "stats: show device statistics",
	.tokens = {
		(void *)&cmd_device_stats_,
		(void *)&cmd_device_bdf,
		(void *)&cmd_queue_id,
		NULL,
	},
};

/* *** QUIT *** */
struct cmd_quit_result {
	cmdline_fixed_string_t quit;
};

static void cmd_quit_parsed(__rte_unused void *parsed_result,
		struct cmdline *cl,
		__rte_unused void *data)
{
	vdpa_sample_quit();
	cmdline_quit(cl);
}

cmdline_parse_token_string_t cmd_quit_quit =
	TOKEN_STRING_INITIALIZER(struct cmd_quit_result, quit, "quit");

cmdline_parse_inst_t cmd_quit = {
	.f = cmd_quit_parsed,
	.data = NULL,
	.help_str = "quit: exit application",
	.tokens = {
		(void *)&cmd_quit_quit,
		NULL,
	},
};
cmdline_parse_ctx_t main_ctx[] = {
	(cmdline_parse_inst_t *)&cmd_help,
	(cmdline_parse_inst_t *)&cmd_list_vdpa_devices,
	(cmdline_parse_inst_t *)&cmd_create_vdpa_port,
	(cmdline_parse_inst_t *)&cmd_device_stats,
	(cmdline_parse_inst_t *)&cmd_quit,
	NULL,
};

int
main(int argc, char *argv[])
{
	char ch;
	int ret;
	struct cmdline *cl;
	struct rte_vdpa_device *vdev;
	struct rte_device *dev;
	struct rte_dev_iterator dev_iter;
	rte_uuid_t vf_token;
	struct rte_device *tdev = NULL;
	sigset_t set;
	pthread_t thread_s;

	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	ret = pthread_sigmask(SIG_BLOCK, &set, NULL);
	if (ret != 0)
		printf("Set sig mask fail");

	ret = pthread_create(&thread_s,NULL,&sig_thread,(void*)&set);
	if (ret!= 0)
		rte_exit(EXIT_FAILURE, "sig thread create failed\n");

	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "eal init failed\n");
	argc -= ret;
	argv += ret;
	eal_start = true;

	rte_eal_vfio_get_vf_token(vf_token);
	/* Generate VF token firstly, if the user-configured NULL*/
	if (rte_uuid_is_null(vf_token)) {
		uuid_generate(vf_token);
		rte_eal_vfio_set_vf_token(vf_token);
	}
	ret = vdpa_rpc_start(&vdpa_rpc_ctx);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "rpc init failed\n");
	rpc_start = true;

	ret = parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "invalid argument\n");

	if (interactive == 1) {
		cl = cmdline_stdin_new(main_ctx, "vdpa> ");
		if (cl == NULL)
			rte_panic("Cannot create cmdline instance\n");
		cmdline_interact(cl);
		cmdline_stdin_exit(cl);
	} else {
		RTE_DEV_FOREACH(dev, "class=vdpa", &dev_iter) {
			vdev = rte_vdpa_find_device_by_name(dev->name);
			if (vdev == NULL) {
				rte_panic("Failed to find vDPA dev for %s\n",
						dev->name);
			}
			vports[devcnt].dev = vdev;
			snprintf(vports[devcnt].ifname, MAX_PATH_LEN, "%s%d",
					iface, devcnt);

			start_vdpa(&vports[devcnt]);
			devcnt++;
		}

		printf("enter \'q\' to quit\n");
		while (scanf("%c", &ch)) {
			if (ch == 'q')
				break;
			while (ch != '\n') {
				if (scanf("%c", &ch))
					printf("%c", ch);
			}
			printf("enter \'q\' to quit\n");
		}
	}
	vdpa_rpc_stop(&vdpa_rpc_ctx);
	vdpa_sample_quit();

	RTE_DEV_FOREACH_SAFE(dev, "class=vdpa", &dev_iter, tdev) {
		rte_dev_remove(dev);
	}

	RTE_DEV_FOREACH_SAFE(dev, "bus=pci", &dev_iter, tdev) {
		rte_dev_remove(dev);
	}

	printf("\n Remove default container fd \r\n");
	if (rte_vfio_container_destroy(RTE_VFIO_DEFAULT_CONTAINER_FD) != 0) {
		printf("\n Default container fd remove fail \r\n");
	}

	/* clean up the EAL */
	rte_eal_cleanup();

	return 0;
}
