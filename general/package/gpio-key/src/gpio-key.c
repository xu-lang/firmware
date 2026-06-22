#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define GPIO_EXPORT_PATH "/sys/class/gpio/export"
#define GPIO_UNEXPORT_PATH "/sys/class/gpio/unexport"
#define GPIO_BASE_PATH "/sys/class/gpio/gpio%d"
#define DOUBLE_CLICK_TIMEOUT_MS 200

typedef enum {
	KEY_EVENT_NONE = 0,
	KEY_EVENT_CLICK,
	KEY_EVENT_DOUBLE_CLICK,
	KEY_EVENT_HOLD_1S,
	KEY_EVENT_HOLD_3S,
	KEY_EVENT_HOLD_5S,
	KEY_EVENT_HOLD_10S,
} key_event_t;

static volatile sig_atomic_t running = 1;
static int g_pin;
static int g_debounce_ms = 50;
static const char *g_event_cmds[KEY_EVENT_HOLD_10S + 1];

static struct {
	int state;
	struct timespec press_time;
	struct timespec release_time;
	int click_count;
} key_state;

static void on_click(void)
{
	printf("[EVENT] Single click\n");
}

static void on_double_click(void)
{
	printf("[EVENT] Double click\n");
}

static void on_hold_1s(void)
{
	printf("[EVENT] Hold 1 second\n");
}

static void on_hold_3s(void)
{
	printf("[EVENT] Hold 3 seconds\n");
}

static void on_hold_5s(void)
{
	printf("[EVENT] Hold 5 seconds\n");
}

static void on_hold_10s(void)
{
	printf("[EVENT] Hold 10 seconds\n");
}

static void run_event_command(key_event_t event)
{
	int ret;

	if (!g_event_cmds[event])
		return;

	printf("[CMD] %s\n", g_event_cmds[event]);
	fflush(stdout);
	ret = system(g_event_cmds[event]);
	if (ret == -1)
		perror("system");
}

static long long timespec_diff_ms(const struct timespec *start,
					 const struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) * 1000LL +
	       (end->tv_nsec - start->tv_nsec) / 1000000LL;
}

static void dispatch_key_event(key_event_t event)
{
	switch (event) {
	case KEY_EVENT_CLICK:
		on_click();
		run_event_command(event);
		break;
	case KEY_EVENT_DOUBLE_CLICK:
		on_double_click();
		run_event_command(event);
		break;
	case KEY_EVENT_HOLD_1S:
		on_hold_1s();
		run_event_command(event);
		break;
	case KEY_EVENT_HOLD_3S:
		on_hold_3s();
		run_event_command(event);
		break;
	case KEY_EVENT_HOLD_5S:
		on_hold_5s();
		run_event_command(event);
		break;
	case KEY_EVENT_HOLD_10S:
		on_hold_10s();
		run_event_command(event);
		break;
	default:
		break;
	}
}

static void handle_key_press(void)
{
	if (key_state.state == 2)
		key_state.click_count = 2;

	clock_gettime(CLOCK_MONOTONIC, &key_state.press_time);
	key_state.state = 1;
	printf("[STATE] Key pressed\n");
}

static void handle_key_release(void)
{
	struct timespec now;
	long long press_duration;

	clock_gettime(CLOCK_MONOTONIC, &now);
	press_duration = timespec_diff_ms(&key_state.press_time, &now);

	printf("[STATE] Key released, duration: %lldms\n", press_duration);

	if (press_duration < 1000) {
		if (key_state.click_count == 2) {
			key_state.state = 0;
			key_state.click_count = 0;
			dispatch_key_event(KEY_EVENT_DOUBLE_CLICK);
			return;
		}

		key_state.release_time = now;
		key_state.click_count = 1;
		key_state.state = 2;
		printf("[STATE] Waiting for double click (%dms timeout)...\n",
		       DOUBLE_CLICK_TIMEOUT_MS);
		return;
	}

	key_state.state = 0;
	key_state.click_count = 0;

	if (press_duration >= 10000)
		dispatch_key_event(KEY_EVENT_HOLD_10S);
	else if (press_duration >= 5000)
		dispatch_key_event(KEY_EVENT_HOLD_5S);
	else if (press_duration >= 3000)
		dispatch_key_event(KEY_EVENT_HOLD_3S);
	else
		dispatch_key_event(KEY_EVENT_HOLD_1S);
}

static void handle_double_click_timeout(void)
{
	if (key_state.state != 2)
		return;

	key_state.state = 0;
	key_state.click_count = 0;
	dispatch_key_event(KEY_EVENT_CLICK);
	printf("[STATE] Double click timeout, triggering single click\n");
}

static void print_help(const char *prog)
{
	printf("Usage: %s <GPIO pin number> [options]\n", prog);
	printf("\n");
	printf("Options:\n");
	printf("  -d <ms>      Debounce time in milliseconds (default: 50ms)\n");
	printf("  -e <edge>    Trigger edge: rising / falling / both (default: both)\n");
	printf("  --click <cmd>       Run command on single click\n");
	printf("  --double <cmd>      Run command on double click\n");
	printf("  --hold-1s <cmd>     Run command on hold >= 1 second\n");
	printf("  --hold-3s <cmd>     Run command on hold >= 3 seconds\n");
	printf("  --hold-5s <cmd>     Run command on hold >= 5 seconds\n");
	printf("  --hold-10s <cmd>    Run command on hold >= 10 seconds\n");
	printf("  -h           Show this help message\n");
	printf("\n");
	printf("Key events:\n");
	printf("  Single click     Press and release within 1 second\n");
	printf("  Double click     Two single clicks within 200ms\n");
	printf("  Hold 1s          Press and hold >= 1 second, then release\n");
	printf("  Hold 3s          Press and hold >= 3 seconds, then release\n");
	printf("  Hold 5s          Press and hold >= 5 seconds, then release\n");
	printf("  Hold 10s         Press and hold >= 10 seconds, then release\n");
	printf("\n");
	printf("Examples:\n");
	printf("  %s 19                   Monitor GPIO19 with default settings\n", prog);
	printf("  %s 19 -d 30             Set debounce to 30ms\n", prog);
	printf("  %s 19 -e falling        Trigger only on falling edge\n", prog);
	printf("  %s 19 -e rising         Trigger only on rising edge\n", prog);
	printf("  %s 19 --hold-1s './start-ap.sh'\n", prog);
	printf("\n");
	printf("Press Ctrl+C to exit\n");
}

static void signal_handler(int sig)
{
	(void)sig;
	running = 0;
}

static void sleep_ms(int ms)
{
	if (ms > 0)
		usleep(ms * 1000);
}

static int write_to_file(const char *path, const char *value)
{
	int fd;
	ssize_t len;

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		perror("  open");
		return -1;
	}

	len = (ssize_t)strlen(value);
	if (write(fd, value, len) != len) {
		perror("  write");
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

static int gpio_is_exported(int pin)
{
	char path[64];

	snprintf(path, sizeof(path), GPIO_BASE_PATH "/value", pin);
	return access(path, F_OK) == 0;
}

static int gpio_export(int pin)
{
	char buf[16];

	if (gpio_is_exported(pin)) {
		printf("  GPIO%d already exported\n", pin);
		return 0;
	}

	snprintf(buf, sizeof(buf), "%d", pin);
	printf("  Exporting GPIO%d ... ", pin);
	fflush(stdout);
	if (write_to_file(GPIO_EXPORT_PATH, buf) < 0) {
		printf("FAILED\n");
		return -1;
	}

	printf("OK\n");
	sleep_ms(100);
	return 0;
}

static int gpio_unexport(int pin)
{
	char buf[16];

	if (!gpio_is_exported(pin))
		return 0;

	snprintf(buf, sizeof(buf), "%d", pin);
	printf("  Unexporting GPIO%d ... ", pin);
	fflush(stdout);
	if (write_to_file(GPIO_UNEXPORT_PATH, buf) < 0) {
		printf("FAILED\n");
		return -1;
	}

	printf("OK\n");
	return 0;
}

static int gpio_set_direction(int pin, const char *dir)
{
	char path[64];

	snprintf(path, sizeof(path), GPIO_BASE_PATH "/direction", pin);
	printf("  Setting direction: %s ... ", dir);
	fflush(stdout);
	if (write_to_file(path, dir) < 0) {
		printf("FAILED\n");
		return -1;
	}

	printf("OK\n");
	return 0;
}

static int gpio_set_edge(int pin, const char *edge)
{
	char path[64];

	snprintf(path, sizeof(path), GPIO_BASE_PATH "/edge", pin);
	printf("  Setting edge: %s ... ", edge);
	fflush(stdout);
	if (write_to_file(path, edge) < 0) {
		printf("FAILED\n");
		return -1;
	}

	printf("OK\n");
	return 0;
}

static int gpio_get_value(int pin)
{
	char path[64];
	char buf[4];
	int fd;

	snprintf(path, sizeof(path), GPIO_BASE_PATH "/value", pin);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("  open value");
		return -1;
	}

	if (read(fd, buf, sizeof(buf)) < 0) {
		perror("  read value");
		close(fd);
		return -1;
	}

	close(fd);
	return buf[0] == '1' ? 1 : 0;
}

static int gpio_open_value_fd(int pin)
{
	char path[64];
	char buf[4];
	int fd;

	snprintf(path, sizeof(path), GPIO_BASE_PATH "/value", pin);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("  open value for poll");
		return -1;
	}

	(void)read(fd, buf, sizeof(buf));
	return fd;
}

static int is_numeric(const char *value)
{
	int i;

	for (i = 0; value[i]; i++) {
		if (!isdigit((unsigned char)value[i]))
			return 0;
	}

	return i > 0;
}

static int parse_args(int argc, char **argv, int *pin, int *debounce,
			      char **edge)
{
	int i;

	if (argc < 2) {
		fprintf(stderr, "ERROR: Missing GPIO pin number\n\n");
		print_help(argv[0]);
		return -1;
	}

	if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
		print_help(argv[0]);
		return 1;
	}

	if (!is_numeric(argv[1])) {
		fprintf(stderr, "ERROR: Pin number must be numeric\n\n");
		print_help(argv[0]);
		return -1;
	}

	*pin = atoi(argv[1]);
	*debounce = 50;
	*edge = "both";

	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "-d") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "ERROR: -d requires a millisecond value\n");
				return -1;
			}
			if (!is_numeric(argv[i + 1])) {
				fprintf(stderr, "ERROR: Debounce value must be numeric\n");
				return -1;
			}
			*debounce = atoi(argv[i + 1]);
			i++;
		} else if (strcmp(argv[i], "-e") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "ERROR: -e requires an edge type\n");
				return -1;
			}
			if (strcmp(argv[i + 1], "rising") != 0 &&
			    strcmp(argv[i + 1], "falling") != 0 &&
			    strcmp(argv[i + 1], "both") != 0) {
				fprintf(stderr,
					"ERROR: Edge type must be rising / falling / both\n");
				return -1;
			}
			*edge = argv[i + 1];
			i++;
		} else if (strcmp(argv[i], "-h") == 0 ||
			   strcmp(argv[i], "--help") == 0) {
			print_help(argv[0]);
			return 1;
		} else if (strcmp(argv[i], "--click") == 0 ||
			   strcmp(argv[i], "--double") == 0 ||
			   strcmp(argv[i], "--hold-1s") == 0 ||
			   strcmp(argv[i], "--hold-3s") == 0 ||
			   strcmp(argv[i], "--hold-5s") == 0 ||
			   strcmp(argv[i], "--hold-10s") == 0) {
			key_event_t event;

			if (i + 1 >= argc) {
				fprintf(stderr, "ERROR: %s requires a command\n", argv[i]);
				return -1;
			}

			if (strcmp(argv[i], "--click") == 0)
				event = KEY_EVENT_CLICK;
			else if (strcmp(argv[i], "--double") == 0)
				event = KEY_EVENT_DOUBLE_CLICK;
			else if (strcmp(argv[i], "--hold-1s") == 0)
				event = KEY_EVENT_HOLD_1S;
			else if (strcmp(argv[i], "--hold-3s") == 0)
				event = KEY_EVENT_HOLD_3S;
			else if (strcmp(argv[i], "--hold-5s") == 0)
				event = KEY_EVENT_HOLD_5S;
			else
				event = KEY_EVENT_HOLD_10S;

			g_event_cmds[event] = argv[i + 1];
			i++;
		} else {
			fprintf(stderr, "ERROR: Unknown option: %s\n", argv[i]);
			print_help(argv[0]);
			return -1;
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct pollfd fds[1];
	char *edge_type;
	int current_value;
	int fd;
	int last_value;
	int parse_ret;
	int alternate_edge = 0;
	int ret;

	parse_ret = parse_args(argc, argv, &g_pin, &g_debounce_ms, &edge_type);
	if (parse_ret != 0)
		return parse_ret > 0 ? EXIT_SUCCESS : EXIT_FAILURE;

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	printf("\n");
	printf("============================================\n");
	printf("  GPIO Key Listener (Click / Double / Hold)\n");
	printf("============================================\n");
	printf("  Pin:        GPIO%d\n", g_pin);
	printf("  Debounce:   %dms\n", g_debounce_ms);
	printf("  Edge:       %s\n", edge_type);
	printf("  Press Ctrl+C to exit\n");
	printf("============================================\n\n");

	if (gpio_export(g_pin) < 0) {
		fprintf(stderr, "\n[ERROR] Failed to export GPIO\n");
		return EXIT_FAILURE;
	}

	if (gpio_set_direction(g_pin, "in") < 0) {
		fprintf(stderr, "\n[ERROR] Failed to set direction\n");
		gpio_unexport(g_pin);
		return EXIT_FAILURE;
	}

	last_value = gpio_get_value(g_pin);
	if (last_value < 0) {
		gpio_unexport(g_pin);
		return EXIT_FAILURE;
	}

	if (gpio_set_edge(g_pin, edge_type) < 0) {
		if (strcmp(edge_type, "both") != 0) {
			fprintf(stderr, "\n[ERROR] Failed to set edge\n");
			gpio_unexport(g_pin);
			return EXIT_FAILURE;
		}

		alternate_edge = 1;
		edge_type = last_value ? "falling" : "rising";
		fprintf(stderr,
			"\n[WARN] Edge 'both' is unsupported, using alternating %s/rising/falling interrupts\n",
			edge_type);
		if (gpio_set_edge(g_pin, edge_type) < 0) {
			fprintf(stderr, "\n[ERROR] Failed to set fallback edge\n");
			gpio_unexport(g_pin);
			return EXIT_FAILURE;
		}
	}

	fd = gpio_open_value_fd(g_pin);
	if (fd < 0) {
		gpio_unexport(g_pin);
		return EXIT_FAILURE;
	}

	fds[0].fd = fd;
	fds[0].events = POLLPRI;

	printf("\n[WAITING] Monitoring GPIO%d ...\n\n", g_pin);

	while (running) {
		long long poll_timeout = -1;

		if (key_state.state == 2) {
			struct timespec now;
			long long elapsed;

			clock_gettime(CLOCK_MONOTONIC, &now);
			elapsed = timespec_diff_ms(&key_state.release_time, &now);
			if (elapsed >= DOUBLE_CLICK_TIMEOUT_MS) {
				handle_double_click_timeout();
			} else {
				poll_timeout = DOUBLE_CLICK_TIMEOUT_MS - elapsed;
			}
		}

		ret = poll(fds, 1, poll_timeout);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}

		if (fds[0].revents & POLLPRI) {
			char buf[4];

			sleep_ms(g_debounce_ms);

			current_value = gpio_get_value(g_pin);
			if (current_value < 0) {
				fprintf(stderr, "Failed to read value\n");
				break;
			}

			if (current_value != last_value) {
				if (current_value == 0)
					handle_key_press();
				else
					handle_key_release();
				last_value = current_value;

				if (alternate_edge) {
					edge_type = current_value ? "falling" : "rising";
					if (gpio_set_edge(g_pin, edge_type) < 0) {
						fprintf(stderr, "Failed to switch edge to %s\n",
							edge_type);
						break;
					}
				}
			}

			lseek(fd, 0, SEEK_SET);
			(void)read(fd, buf, sizeof(buf));
		}

		if (ret == 0 && key_state.state == 2)
			handle_double_click_timeout();
	}

	printf("\n[CLEANUP] Exiting...\n");
	close(fd);
	gpio_unexport(g_pin);
	printf("[DONE] Program exited\n");

	return EXIT_SUCCESS;
}
