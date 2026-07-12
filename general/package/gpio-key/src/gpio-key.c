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
#define DEFAULT_CONFIG_PATH "/etc/gpio-key.conf"
#define DOUBLE_CLICK_TIMEOUT_MS 200
#define LED_DISABLED_PIN -1
#define LED_HOLD_FLASH_HALF_MS 100

typedef enum {
	KEY_EVENT_NONE = 0,
	KEY_EVENT_CLICK,
	KEY_EVENT_DOUBLE_CLICK,
	KEY_EVENT_HOLD_1S,
	KEY_EVENT_HOLD_2S,
	KEY_EVENT_HOLD_3S,
	KEY_EVENT_HOLD_5S,
	KEY_EVENT_HOLD_10S,
} key_event_t;

static volatile sig_atomic_t running = 1;
static int g_pin = 8;
static int g_debounce_ms = 50;
static int g_led_pin = 6;
static int g_led_active_low = 1;
static int g_enabled = 1;
static char g_edge_type[16] = "both";
static const char *g_event_cmds[KEY_EVENT_HOLD_10S + 1] = {
	[KEY_EVENT_HOLD_1S] = "/bin/ap-control start",
	[KEY_EVENT_HOLD_3S] = "/bin/ap-control stop",
};

static struct {
	int state;
	struct timespec press_time;
	struct timespec release_time;
	int click_count;
} key_state;

static struct {
	int is_on;
	int next_second;
	int release_hold;
	struct timespec next_change;
} led_state;

static void led_start_press(const struct timespec *now);
static void led_release_hold(const struct timespec *now);
static void led_stop(void);
static long long led_update(const struct timespec *now);

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

static void on_hold_2s(void)
{
	printf("[EVENT] Hold 2 seconds\n");
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

static void timespec_add_ms(struct timespec *time, int ms)
{
	time->tv_sec += ms / 1000;
	time->tv_nsec += (long)(ms % 1000) * 1000000L;
	if (time->tv_nsec >= 1000000000L) {
		time->tv_sec++;
		time->tv_nsec -= 1000000000L;
	}
}

static long long timespec_until_ms(const struct timespec *deadline,
					 const struct timespec *now)
{
	long long ms = timespec_diff_ms(now, deadline);

	return ms > 0 ? ms : 0;
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
	case KEY_EVENT_HOLD_2S:
		on_hold_2s();
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
	struct timespec now;

	if (key_state.state == 2)
		key_state.click_count = 2;

	clock_gettime(CLOCK_MONOTONIC, &now);
	key_state.press_time = now;
	key_state.state = 1;
	led_start_press(&now);
	printf("[STATE] Key pressed\n");
}

static void handle_key_release(void)
{
	struct timespec now;
	long long press_duration;

	clock_gettime(CLOCK_MONOTONIC, &now);
	press_duration = timespec_diff_ms(&key_state.press_time, &now);

	printf("[STATE] Key released, duration: %lldms\n", press_duration);
	led_release_hold(&now);

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
	else if (press_duration >= 2000)
		dispatch_key_event(KEY_EVENT_HOLD_2S);
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
	printf("Usage: %s [GPIO pin number] [options]\n", prog);
	printf("\n");
	printf("Options:\n");
	printf("  -c, --config <file>  Config file (default: /etc/gpio-key.conf)\n");
	printf("  -d <ms>      Debounce time in milliseconds (default: 50ms)\n");
	printf("  -e <edge>    Trigger edge: rising / falling / both (default: both)\n");
	printf("  --led <pin>          Blink once per held second; after release stay off 1s, then on\n");
	printf("  --led-active-low     LED is on when GPIO output is low\n");
	printf("  --click <cmd>       Run command on single click\n");
	printf("  --double <cmd>      Run command on double click\n");
	printf("  --hold-1s <cmd>     Run command on hold >= 1 second\n");
	printf("  --hold-2s <cmd>     Run command on hold >= 2 seconds\n");
	printf("  --hold-3s <cmd>     Run command on hold >= 3 seconds\n");
	printf("  --hold-5s <cmd>     Run command on hold >= 5 seconds\n");
	printf("  --hold-10s <cmd>    Run command on hold >= 10 seconds\n");
	printf("  -h           Show this help message\n");
	printf("\n");
	printf("Key events:\n");
	printf("  Single click     Press and release within 1 second\n");
	printf("  Double click     Two single clicks within 200ms\n");
	printf("  Hold 1s          Press and hold >= 1 second, then release\n");
	printf("  Hold 2s          Press and hold >= 2 seconds, then release\n");
	printf("  Hold 3s          Press and hold >= 3 seconds, then release\n");
	printf("  Hold 5s          Press and hold >= 5 seconds, then release\n");
	printf("  Hold 10s         Press and hold >= 10 seconds, then release\n");
	printf("\n");
	printf("Examples:\n");
	printf("  %s                     Load /etc/gpio-key.conf\n", prog);
	printf("  %s -c /tmp/key.conf    Load custom config\n", prog);
	printf("  %s 19                   Monitor GPIO19 with default settings\n", prog);
	printf("  %s 19 -d 30             Set debounce to 30ms\n", prog);
	printf("  %s 19 -e falling        Trigger only on falling edge\n", prog);
	printf("  %s 19 -e rising         Trigger only on rising edge\n", prog);
	printf("  %s 19 --led 20          Use GPIO20 as hold-time indicator LED\n", prog);
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

static int gpio_set_value(int pin, int value)
{
	char path[64];

	snprintf(path, sizeof(path), GPIO_BASE_PATH "/value", pin);
	return write_to_file(path, value ? "1" : "0");
}

static int led_write(int on)
{
	int value;

	if (g_led_pin == LED_DISABLED_PIN)
		return 0;

	value = g_led_active_low ? !on : on;
	if (gpio_set_value(g_led_pin, value) < 0)
		return -1;
	led_state.is_on = on;
	return 0;
}

static void led_stop(void)
{
	if (g_led_pin == LED_DISABLED_PIN)
		return;

	(void)led_write(0);
	led_state.next_second = 0;
	led_state.release_hold = 0;
}

static void led_release_hold(const struct timespec *now)
{
	if (g_led_pin == LED_DISABLED_PIN)
		return;

	(void)led_write(0);
	led_state.next_second = 0;
	led_state.release_hold = 1;
	led_state.next_change = *now;
	timespec_add_ms(&led_state.next_change, 1000);
}

static void led_start_press(const struct timespec *now)
{
	if (g_led_pin == LED_DISABLED_PIN)
		return;

	(void)led_write(0);
	led_state.next_second = 1;
	led_state.release_hold = 0;
	led_state.next_change = *now;
	timespec_add_ms(&led_state.next_change,
				 1000 - LED_HOLD_FLASH_HALF_MS);
}

static long long led_update(const struct timespec *now)
{
	long long elapsed;

	if (g_led_pin == LED_DISABLED_PIN)
		return -1;

	if (led_state.release_hold) {
		if (timespec_diff_ms(now, &led_state.next_change) > 0)
			return timespec_until_ms(&led_state.next_change, now);

		(void)led_write(1);
		led_state.release_hold = 0;
		return -1;
	}

	if (key_state.state != 1)
		return -1;

	if (timespec_diff_ms(now, &led_state.next_change) > 0)
		return timespec_until_ms(&led_state.next_change, now);

	if (led_state.is_on) {
		(void)led_write(0);
		led_state.next_second++;
		led_state.next_change = key_state.press_time;
		timespec_add_ms(&led_state.next_change,
				 led_state.next_second * 1000 - LED_HOLD_FLASH_HALF_MS);
		return timespec_until_ms(&led_state.next_change, now);
	}

	elapsed = timespec_diff_ms(&key_state.press_time, now);
	if (elapsed >= led_state.next_second * 1000LL - LED_HOLD_FLASH_HALF_MS) {
		printf("[LED] Hold %ds\n", led_state.next_second);
		(void)led_write(1);
		led_state.next_change = key_state.press_time;
		timespec_add_ms(&led_state.next_change,
				 led_state.next_second * 1000 + LED_HOLD_FLASH_HALF_MS);
	}

	return timespec_until_ms(&led_state.next_change, now);
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

static char *trim(char *value)
{
	char *end;

	while (isspace((unsigned char)*value))
		value++;

	if (*value == '\0')
		return value;

	end = value + strlen(value) - 1;
	while (end > value && isspace((unsigned char)*end))
		*end-- = '\0';

	return value;
}

static int parse_bool(const char *value)
{
	if (strcmp(value, "1") == 0 || strcasecmp(value, "yes") == 0 ||
	    strcasecmp(value, "true") == 0 || strcasecmp(value, "on") == 0)
		return 1;

	return 0;
}

static int set_event_command(const char *key, const char *value)
{
	key_event_t event;

	if (strcmp(key, "click") == 0)
		event = KEY_EVENT_CLICK;
	else if (strcmp(key, "double") == 0 || strcmp(key, "double_click") == 0)
		event = KEY_EVENT_DOUBLE_CLICK;
	else if (strcmp(key, "hold_1s") == 0 || strcmp(key, "hold-1s") == 0)
		event = KEY_EVENT_HOLD_1S;
	else if (strcmp(key, "hold_2s") == 0 || strcmp(key, "hold-2s") == 0)
		event = KEY_EVENT_HOLD_2S;
	else if (strcmp(key, "hold_3s") == 0 || strcmp(key, "hold-3s") == 0)
		event = KEY_EVENT_HOLD_3S;
	else if (strcmp(key, "hold_5s") == 0 || strcmp(key, "hold-5s") == 0)
		event = KEY_EVENT_HOLD_5S;
	else if (strcmp(key, "hold_10s") == 0 || strcmp(key, "hold-10s") == 0)
		event = KEY_EVENT_HOLD_10S;
	else
		return 0;

	if (!*value) {
		g_event_cmds[event] = NULL;
		return 1;
	}

	g_event_cmds[event] = strdup(value);
	return g_event_cmds[event] ? 1 : -1;
}

static int apply_config_value(const char *key, const char *value)
{
	if (strcmp(key, "enabled") == 0)
		g_enabled = parse_bool(value);
	else if (strcmp(key, "pin") == 0 || strcmp(key, "gpio") == 0) {
		if (!is_numeric(value))
			return -1;
		g_pin = atoi(value);
	} else if (strcmp(key, "debounce") == 0 || strcmp(key, "debounce_ms") == 0) {
		if (!is_numeric(value))
			return -1;
		g_debounce_ms = atoi(value);
	} else if (strcmp(key, "edge") == 0) {
		if (strcmp(value, "rising") != 0 && strcmp(value, "falling") != 0 &&
		    strcmp(value, "both") != 0)
			return -1;
		strncpy(g_edge_type, value, sizeof(g_edge_type) - 1);
		g_edge_type[sizeof(g_edge_type) - 1] = '\0';
	} else if (strcmp(key, "led") == 0 || strcmp(key, "led_pin") == 0) {
		if (!is_numeric(value))
			return -1;
		g_led_pin = atoi(value);
	} else if (strcmp(key, "led_active_low") == 0)
		g_led_active_low = parse_bool(value);
	else {
		int ret = set_event_command(key, value);

		if (ret < 0)
			return -1;
	}

	return 0;
}

static int load_config_file(const char *path, int required)
{
	FILE *fp;
	char line[512];
	int lineno = 0;

	fp = fopen(path, "r");
	if (!fp) {
		if (required) {
			fprintf(stderr, "ERROR: Failed to open config %s: %s\n", path,
				strerror(errno));
			return -1;
		}
		return 0;
	}

	while (fgets(line, sizeof(line), fp)) {
		char *key, *value, *sep;

		lineno++;
		key = trim(line);
		if (*key == '\0' || *key == '#')
			continue;

		sep = strchr(key, '=');
		if (!sep) {
			fprintf(stderr, "ERROR: Invalid config line %d in %s\n", lineno, path);
			fclose(fp);
			return -1;
		}

		*sep = '\0';
		value = trim(sep + 1);
		key = trim(key);

		if (apply_config_value(key, value) < 0) {
			fprintf(stderr, "ERROR: Invalid value for '%s' at %s:%d\n",
				key, path, lineno);
			fclose(fp);
			return -1;
		}
	}

	fclose(fp);
	return 0;
}

static int parse_args(int argc, char **argv, int *pin, int *debounce,
			      char **edge)
{
	int i;
	const char *config_path = DEFAULT_CONFIG_PATH;
	int config_required = 0;
	int use_config = argc == 1;
	int loaded_config = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			print_help(argv[0]);
			return 1;
		} else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "ERROR: %s requires a config path\n", argv[i]);
				return -1;
			}
			config_path = argv[i + 1];
			config_required = 1;
			use_config = 1;
			i++;
		}
	}

	if (use_config) {
		if (load_config_file(config_path, config_required) < 0)
			return -1;
		loaded_config = access(config_path, F_OK) == 0;
	}

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
			i++;
		} else if (strcmp(argv[i], "-d") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "ERROR: -d requires a millisecond value\n");
				return -1;
			}
			if (!is_numeric(argv[i + 1])) {
				fprintf(stderr, "ERROR: Debounce value must be numeric\n");
				return -1;
			}
			g_debounce_ms = atoi(argv[i + 1]);
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
			strncpy(g_edge_type, argv[i + 1], sizeof(g_edge_type) - 1);
			g_edge_type[sizeof(g_edge_type) - 1] = '\0';
			i++;
		} else if (strcmp(argv[i], "-h") == 0 ||
			   strcmp(argv[i], "--help") == 0) {
			print_help(argv[0]);
			return 1;
		} else if (strcmp(argv[i], "--led") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "ERROR: --led requires a GPIO pin number\n");
				return -1;
			}
			if (!is_numeric(argv[i + 1])) {
				fprintf(stderr, "ERROR: LED pin number must be numeric\n");
				return -1;
			}
			g_led_pin = atoi(argv[i + 1]);
			i++;
		} else if (strcmp(argv[i], "--led-active-low") == 0) {
			g_led_active_low = 1;
		} else if (strcmp(argv[i], "--click") == 0 ||
			   strcmp(argv[i], "--double") == 0 ||
			   strcmp(argv[i], "--hold-1s") == 0 ||
			   strcmp(argv[i], "--hold-2s") == 0 ||
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
			else if (strcmp(argv[i], "--hold-2s") == 0)
				event = KEY_EVENT_HOLD_2S;
			else if (strcmp(argv[i], "--hold-3s") == 0)
				event = KEY_EVENT_HOLD_3S;
			else if (strcmp(argv[i], "--hold-5s") == 0)
				event = KEY_EVENT_HOLD_5S;
			else
				event = KEY_EVENT_HOLD_10S;

			g_event_cmds[event] = argv[i + 1];
			i++;
		} else if (is_numeric(argv[i])) {
			g_pin = atoi(argv[i]);
		} else {
			fprintf(stderr, "ERROR: Unknown option: %s\n", argv[i]);
			print_help(argv[0]);
			return -1;
		}
	}

	if (!g_enabled) {
		printf("gpio-key disabled by config\n");
		return 1;
	}

	if (g_pin < 0) {
		fprintf(stderr, "ERROR: Missing GPIO pin number%s\n\n",
			loaded_config ? " in config" : "");
		print_help(argv[0]);
		return -1;
	}

	*pin = g_pin;
	*debounce = g_debounce_ms;
	*edge = g_edge_type;

	if (g_led_pin == *pin) {
		fprintf(stderr, "ERROR: LED pin must be different from key pin\n");
		return -1;
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
	int led_timeout;
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
	if (g_led_pin != LED_DISABLED_PIN)
		printf("  LED:        GPIO%d%s\n", g_led_pin,
		       g_led_active_low ? " (active low)" : "");
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

	if (g_led_pin != LED_DISABLED_PIN) {
		if (gpio_export(g_led_pin) < 0) {
			fprintf(stderr, "\n[ERROR] Failed to export LED GPIO\n");
			gpio_unexport(g_pin);
			return EXIT_FAILURE;
		}

		if (gpio_set_direction(g_led_pin, "out") < 0) {
			fprintf(stderr, "\n[ERROR] Failed to set LED direction\n");
			gpio_unexport(g_led_pin);
			gpio_unexport(g_pin);
			return EXIT_FAILURE;
		}

		if (led_write(1) < 0) {
			fprintf(stderr, "\n[ERROR] Failed to set LED value\n");
			gpio_unexport(g_led_pin);
			gpio_unexport(g_pin);
			return EXIT_FAILURE;
		}
	}

	last_value = gpio_get_value(g_pin);
	if (last_value < 0) {
		if (g_led_pin != LED_DISABLED_PIN)
			gpio_unexport(g_led_pin);
		gpio_unexport(g_pin);
		return EXIT_FAILURE;
	}

	if (gpio_set_edge(g_pin, edge_type) < 0) {
		if (strcmp(edge_type, "both") != 0) {
			fprintf(stderr, "\n[ERROR] Failed to set edge\n");
			if (g_led_pin != LED_DISABLED_PIN)
				gpio_unexport(g_led_pin);
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
			if (g_led_pin != LED_DISABLED_PIN)
				gpio_unexport(g_led_pin);
			gpio_unexport(g_pin);
			return EXIT_FAILURE;
		}
	}

	fd = gpio_open_value_fd(g_pin);
	if (fd < 0) {
		if (g_led_pin != LED_DISABLED_PIN)
			gpio_unexport(g_led_pin);
		gpio_unexport(g_pin);
		return EXIT_FAILURE;
	}

	fds[0].fd = fd;
	fds[0].events = POLLPRI;

	printf("\n[WAITING] Monitoring GPIO%d ...\n\n", g_pin);

	while (running) {
		long long poll_timeout = -1;
		struct timespec now;

		clock_gettime(CLOCK_MONOTONIC, &now);
		led_timeout = (int)led_update(&now);
		if (led_timeout >= 0)
			poll_timeout = led_timeout;

		if (key_state.state == 2) {
			long long elapsed;

			elapsed = timespec_diff_ms(&key_state.release_time, &now);
			if (elapsed >= DOUBLE_CLICK_TIMEOUT_MS) {
				handle_double_click_timeout();
			} else {
				long long click_timeout = DOUBLE_CLICK_TIMEOUT_MS - elapsed;

				if (poll_timeout < 0 || click_timeout < poll_timeout)
					poll_timeout = click_timeout;
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
	led_stop();
	close(fd);
	if (g_led_pin != LED_DISABLED_PIN)
		gpio_unexport(g_led_pin);
	gpio_unexport(g_pin);
	printf("[DONE] Program exited\n");

	return EXIT_SUCCESS;
}
