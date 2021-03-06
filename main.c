#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <curses.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>
#include <errno.h>

typedef uint8_t cell_t;

#define min(a,b) ((a<b)?a:b)
#define max(a,b) ((a<b)?b:a)
#define CELL_COUNT 4096
#define CELL_SIZE sizeof(cell_t)
#define CELL_BUFFER_SIZE (CELL_COUNT * sizeof(cell_t))
#define MAX_DELAY 500000
#define MIN_DELAY 10
#define DELAY_INCREMENT MIN_DELAY

#define OUTPUT_BUFFER_SIZE 0x2000
#define CODE_BUFFER_SIZE OUTPUT_BUFFER_SIZE

static struct {
	unsigned int step_by_step:1;
	unsigned int did_resize:1;
	unsigned int next_instruction:1;
	unsigned int no_curses:1;
	unsigned int did_get_to_eof:1;
	unsigned int should_read_input:1;
	unsigned int execution_completed:1;
} flags;
static cell_t *first_cell;
static cell_t *current_cell_pt;
static WINDOW *code_window;
static WINDOW *output_window;
static WINDOW *cells_window;
//static WINDOW *input_window;
static WINDOW *status_window;
static char *output_buffer;
static char *code;
static char *instruction_pt;
static useconds_t delay;
static int ips;
static char *program_path;
static pthread_t bf_thread;
static pthread_mutex_t instruction_pt_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t input_cond = PTHREAD_COND_INITIALIZER;
static long code_len;
static char *code_buffer;

void handle_resize(int signal) {
	flags.did_resize = 1;
}

static void bf_putc(int c) {
	if (flags.no_curses) {
		putc(c, stdout);
	}
	else {
		pthread_mutex_lock(&output_lock);
		waddch(output_window, c);
		long len = strlen(output_buffer);
		char *old_output_pt = output_buffer;
		if (len >= (OUTPUT_BUFFER_SIZE - 1)) old_output_pt++;
		sprintf(output_buffer, "%s%c", old_output_pt, (char)c);
		pthread_mutex_unlock(&output_lock);
		wrefresh(output_window);
	}
}

static void *execute_bf_code(void *arg) {
#define return { flags.execution_completed = 1; return NULL; }
	time_t start_time;
	int executed_instruction_count = 0;
	char input = 0;
	flags.execution_completed = 0;
	while ((input = *instruction_pt) != 0) {
		if (!executed_instruction_count) time(&start_time);
		if (delay) usleep(delay);
		if (!flags.did_get_to_eof && (!flags.step_by_step || flags.next_instruction)) {
			flags.next_instruction = 0;
			pthread_mutex_lock(&instruction_pt_lock);
			instruction_pt++;
			pthread_mutex_unlock(&instruction_pt_lock);
			char *tmp_instruction_pt = instruction_pt;
			int8_t multiplier = 0;
			switch (input) {
				case '<':
					if (!(current_cell_pt - first_cell)) {
						current_cell_pt = first_cell + CELL_BUFFER_SIZE - 1; // Simulate an overflow
					}
					else {
						current_cell_pt--;
					}
					break;
				case '>':
					if ((current_cell_pt - first_cell + 1) == CELL_BUFFER_SIZE) {
						current_cell_pt = first_cell; // Simulate an overflow
					}
					else {
						current_cell_pt++;
					}
					break;
				case '+':
					(*current_cell_pt)++;
					break;
				case '-':
					(*current_cell_pt)--;
					break;
				case '.':
					bf_putc(*current_cell_pt);
					break;
				case ',':
					if (flags.no_curses) {
						int user_input = fgetc(stdin);
						if (user_input != EOF) {
							*current_cell_pt = (cell_t)user_input;
							// Success
						}
						// ERROR: Received EOF from stdin, not going to modify the cell.
					}
					else {
						flags.should_read_input = 1;
						pthread_mutex_lock(&input_mutex);
						while (flags.should_read_input) {
							pthread_cond_wait(&input_cond, &input_mutex);
						}
						pthread_mutex_unlock(&input_mutex);
					}
					break;
				case '[':
					if (!*current_cell_pt) {
						long counter = 1;
						while (counter > 0) {
							if (tmp_instruction_pt >= (code+code_len-1)) return; // ERROR: Reached EOF before finding a matching bracket.
							input = (*(tmp_instruction_pt++));
							switch (input) {
								case '[': counter++; break;
								case ']': counter--; break;
							}
						}
					}
					pthread_mutex_lock(&instruction_pt_lock);
					instruction_pt = tmp_instruction_pt;
					pthread_mutex_unlock(&instruction_pt_lock);
					break;
				case ']':
					if (*current_cell_pt) {
						long counter = 1;
						tmp_instruction_pt--;
						while (counter > 0) {
							if (tmp_instruction_pt <= code) return; // ERROR: Reached EOF before finding a matching bracket.
							input = (*(--tmp_instruction_pt));
							switch (input) {
								case '[': counter--; break;
								case ']': counter++; break;
							}
						}
					}
					pthread_mutex_lock(&instruction_pt_lock);
					instruction_pt = tmp_instruction_pt;
					pthread_mutex_unlock(&instruction_pt_lock);
					break;

				/* BEGIN: Optimized instructions */
				/* We don't need to use pthread_mutex_lock; optimization is only done without curses */
				case 0x02: // substract <num>
					multiplier = -2;
				case 0x01: // add <num>
					multiplier += 1;
					errno = 0;
					(*current_cell_pt) += ((long)strtol(instruction_pt, &instruction_pt, 10)*multiplier);
					if (errno) return;
					break;
				case 0x03: // move left <num> times
					multiplier = -2;
				case 0x04: // move right <num> times
					multiplier += 1;
					errno = 0;
					int16_t change = (long)strtol(instruction_pt, &instruction_pt, 10) % CELL_COUNT;
					if (errno) return;
					current_cell_pt += (change * multiplier);
					break;
				/* END */

				case '?':
					*current_cell_pt = (cell_t)arc4random_uniform(256);
					break;
				default:
					if (flags.no_curses) {
						fprintf(stderr, "Illegal instruction: %c\n", input);
						exit(4);
					}
					else {
						flags.step_by_step = 1;
					}
			}
		}
		else if (flags.no_curses) {
			return;
		}
		
		executed_instruction_count++;
		
		if ((time(NULL) - start_time) >= 1) {
			ips = executed_instruction_count;
			executed_instruction_count = 0;
		}
	}
	return;
#undef return
}

void redraw_code_window(char *code_pt) {
	if (flags.no_curses) return;
	wclear(code_window);
	int max_x = min(getmaxx(stdscr), CODE_BUFFER_SIZE);
	int cursor_pos = (max_x / 2);
	cursor_pos = cursor_pos + !((cursor_pos * 2) == max_x);
	mvwaddch(code_window, 1, (cursor_pos-1), '^');
	long current_char_index = code_pt-code;
	long offset = 0-min(cursor_pos, current_char_index);
	code_pt += offset;
	for (int i = 0; i < max_x+1; i++) code_buffer[i] = ' ';
	memcpy((code_buffer+(offset+cursor_pos)), code_pt, min(max_x-offset, strlen(code_pt)));
	code_buffer[max_x-1] = 0;
	mvwprintw(code_window, 0, 0, "%s", code_buffer);
	mvwprintw(code_window, 2, 0, "Current character: %ld", code_pt-code-offset+1);
	if ((ips != -1) && !flags.step_by_step && !flags.did_get_to_eof) mvwprintw(code_window, 3, 0, "Delay: %i usecs, %d IPS", delay, ips);
	wrefresh(code_window);
}

void redraw_cells_window(void) {
	if (flags.no_curses) return;
	wclear(cells_window);
	int max_y = min(getmaxy(cells_window), CELL_COUNT);
	int max_x = getmaxx(cells_window);
	for (int i = 0; ((i < max_y) && (i < CELL_COUNT)); i++) mvwprintw(cells_window, i, 3, "......");
	mvwprintw(cells_window, 0, 0, "#%i ", (unsigned int)((current_cell_pt-first_cell)/CELL_SIZE));
	mvwprintw(cells_window, 0, 10, "%03i", (unsigned int)*current_cell_pt);
	for (int i = 0; ((i < max_y-1) && (i < CELL_COUNT)); i++) {
		mvwprintw(cells_window, i+1, 0, "#%i ", i);
		mvwprintw(cells_window, i+1, 10, "%03i", (unsigned int)first_cell[i]);
	}
	wrefresh(cells_window);
}

void refresh_state_window(void) {
	if (flags.no_curses) return;
	wclear(status_window);
	const char *state = "Program is executing...";
	if (getmaxy(stdscr) <= 10) {
		state = "Window too small";
	}
	else if (flags.execution_completed) {
		state = "Execution completed.";
	}
	else if (flags.should_read_input) {
		state = "Waiting for input...";
	}
	mvwprintw(status_window, 0, 0, state);
	wrefresh(status_window);
}

void redraw_screen(void) {
	if (flags.no_curses) return;
	endwin();
	refresh();
	clear();
	int screen_max_x, screen_max_y;
	getmaxyx(stdscr, screen_max_y, screen_max_x);
	code_window = newwin(4, screen_max_x, 0, 0);
	output_window = newwin(screen_max_y-10, screen_max_x-21, 8, 19);
	cells_window = newwin(screen_max_y-8, 13, 6, 2);
	//input_window = newwin(1, screen_max_x, screen_max_y-1, 0);
	status_window = newwin(1, screen_max_x-21, 6, 19);
	{
		int j, i;
		int y[] = { 5, 7, screen_max_y-2 };
		int x[] = { 0, 17, screen_max_x-1 };
		for (i = 0; i < (sizeof(x) / sizeof(*x)); i++) {
			for (j = 6; j < screen_max_y-1; j++) {
				mvwaddch(stdscr, j, x[i], '|');
			}
		}
		for (i = 0; i < (sizeof(y) / sizeof(*y)); i++) {
			for (j = ((i==1) * 17); j < screen_max_x; j++) {
				mvaddch(y[i], j, '-');
			}
			for (j = 0; j < (sizeof(x) / sizeof(*x)); j++) {
				mvwaddch(stdscr, y[i], x[j], '+');
			}
		}
		mvwaddch(stdscr, y[1], x[0], '|');
	}
	wrefresh(stdscr);
	curs_set(0);
	nodelay(stdscr, 1);
	//nodelay(input_window, 0);
	//wtimeout(input_window, -1);
	keypad(stdscr, 1);
	noecho();
	scrollok(output_window, 1);
}

void redraw_output_window(void) {
	if (flags.no_curses) return;
	wclear(output_window);
	pthread_mutex_lock(&output_lock);
	mvwprintw(output_window, 0, 0, output_buffer);
	waddch(output_window, (' ' | A_REVERSE));
	pthread_mutex_unlock(&output_lock);
	wrefresh(output_window);
}

void print_usage(_Bool should_exit) {
	fprintf(stderr,
			"Usage: %s [options] <program>\n"
			"  program: A file containing a brainfuck program or \"-\" to read from stdin.\n"
			"Options:\n"
			"  -n, --no-curses: Don't show the debugger.\n"
			"  -d, --debug-mode: Start in step-by-step mode.\n"
			"  -h, --highest-speed: Run at the highest speed possible.\n"
			"  -u, --delay: Specify a delay between instructions. Has to be a positive multiple of 10.\n"
			"  -s, --support: Allow the usage of brainfuck extensions like '?'.\n"
			"  -x, --exit: Exit after writing creating the output file. Has no effect without -o.\n"
			"  -o, --output: Specify an output file for other file modifiers.\n"
			"  -r, --refresh-rate: Specify the refresh rate for the screen.\n"
			"File Modifiers:\n"
			"  -f, --force-write: Overwrite the file if it exists.\n"
			"  -m, --minify: Minify the program.\n"
			//"  -c, --compress: Compress the program using a custom format.\n"
			, program_path);
	if (should_exit) exit(1);
}

int main(int argc, char **argv) {
	code_len = 0;
	program_path = argv[0];
	delay = 10;
	ips = -1;
	int file_path_index = 0;
	char *valid_characters = malloc(9);
	strcpy(valid_characters, "[]-+<>,.");
	code_buffer = malloc(CODE_BUFFER_SIZE);
	
	// Parse the arguments
	FILE *output_file = NULL;
	char output_modifier = 0;
	_Bool write_and_exit = 0;
	char write_file_mode[3];
	write_file_mode[0] = 'w';
	write_file_mode[1] = 'x';
	write_file_mode[2] = 0;
	{
		static struct option long_opts[] = {
			{"highest-speed", no_argument, NULL, 'h'},
			{"debug-mode", no_argument, NULL, 'd'},
			{"no-curses", no_argument, NULL, 'n'},
			{"minify", no_argument, NULL, 'm'},
			{"delay", required_argument, NULL, 'u'},
			{"support", required_argument, NULL, 's'},
			{"output", required_argument, NULL, 'o'},
			{"exit", no_argument, NULL, 'x'},
			{"force-write", no_argument, NULL, 'f'},
			{"refresh-rate", required_argument, NULL, 'r'},
			//{"compress", no_argument, NULL, 'c'}
		};
		int option = 0;
		_Bool did_manually_set_delay = 0;
		while ((option = getopt_long(argc, argv, "hdnmu:s:o:xcfr:", long_opts, NULL)) != -1) {
			long input;
			switch (option) {
				case 'd':
					flags.step_by_step = 1;
					break;
				case 'n':
					flags.no_curses = 1;
				case 'h':
					if (!did_manually_set_delay) delay = 0;
					break;
				case 'o':
					if (output_file) {
						fprintf(stderr, "Error: You can specify the --output option only once.\n");
						print_usage(1);
					}
					else if (!(output_file = fopen(optarg ?: "", write_file_mode))) {
						fprintf(stderr, "%s: %s: %s\n", program_path, strerror(errno) ?: "Failed to open file for writing", optarg);
						return 1;
					}
					break;
				case 'm':
				case 'c':
					if (output_modifier) {
						fprintf(stderr, "Error: You can specify only one file modifier.\n");
						return 1;
					}
					output_modifier = option;
					break;
				case 'x':
					write_and_exit = 1;
					break;
				case 'u':
					input = strtol(optarg, NULL, 0);
					if ((input <= 0) || (input % DELAY_INCREMENT)) {
						fprintf(stderr, "Error: Invalid delay\n");
						print_usage(1);
					}
					delay = min(input, MAX_DELAY);
					did_manually_set_delay = 1;
					break;
				case 'f': // FIXME: Only works when placed before --output
					write_file_mode[1] = 0;
					break;
				case 's': {
					int optarg_size = strlen(optarg);
					char new_chars[optarg_size+1];
					new_chars[optarg_size] = 0;
					int nc_index = 0;
					for (int opt_index = 0; opt_index < optarg_size; opt_index++) {
						char c = optarg[opt_index];
						if ((c >= 0x20) && (c <= 0x7E)) {
							new_chars[nc_index++] = c;
						}
					}
					new_chars[nc_index] = 0;
					char *old_chars = valid_characters;
					valid_characters = malloc(strlen(old_chars) + strlen(new_chars) + 1);
					sprintf(valid_characters, "%s%s", old_chars, new_chars);
					free(old_chars);
				}
			}
		}
		if (flags.step_by_step && flags.no_curses) {
			fprintf(stderr, "Error: You can't use --debug-mode and --no-curses at the same time.\n");
			return 1;
		}
		else if (optind != (file_path_index = (argc-1))) {
			print_usage(1);
		}
	}
	
	// Copy the program to memory
	FILE *input_file = NULL;
	{
		char *file_path = argv[file_path_index];
		input_file = (!strcmp(file_path, "-")) ? stdin : fopen(file_path, "r");
		if (!input_file) {
			fprintf(stderr, "%s: %s: Failed to open file for reading\n", argv[0], file_path);
			return 2;
		}
		long buffer_size = 256;
		long written_bytes = 0;
		code = malloc(buffer_size);
		while (1) {
			char input = fgetc(input_file);
			if (input == EOF) break;
			short len = strlen(valid_characters);
			for (short i = 0; i < len; i++) {
				if (valid_characters[i] == input) {
					if (written_bytes >= (buffer_size - 1)) {
						buffer_size *= 2;
						code = realloc(code, buffer_size);
					}
					code[written_bytes] = input;
					written_bytes++;
					break;
				}
			}
		}
		code[written_bytes] = 0;
		if (flags.no_curses) {
			char *final_code = calloc(1, (buffer_size+1)*2); // Worst case scenario: Optimized code is 2 times bigger than the original
			char *orig_code = code;
			char *final_code_pt = final_code;
			char orig_c, opt_c;
			while ((orig_c = *(code++))) {
				switch (orig_c) {
					case '+':
						opt_c = 0x01;
						break;
					case '-':
						opt_c = 0x02;
						break;
					case '>':
						opt_c = 0x04;
						break;
					case '<':
						opt_c = 0x03;
						break;
					default:
						opt_c = 0;
				}
				if (opt_c) {
					uint32_t count = 1;
					while ((*code) == orig_c) {
						count++;
						code++;
					}
					char buffer[12];
					sprintf(buffer, "%c%u", opt_c, count);
					sprintf(final_code_pt, "%s", buffer);
					final_code_pt += strlen(buffer);
				}
				else {
					*final_code_pt = orig_c;
					final_code_pt++;
				}
			}
			free(orig_code);
			code = final_code;
		}
		code = realloc(code, strlen(code)+1);
		code_len = strlen(code);
		instruction_pt = code;
	}
	if (output_file) {
		fseek(input_file, SEEK_SET, 0);
		int current_char = -1;
		do {
			current_char = fgetc(input_file);
			uint8_t *algorithm_data = NULL;
			switch (output_modifier) {
				case 'm':
					if (!algorithm_data) {
						algorithm_data = malloc(sizeof(uint8_t));
						*algorithm_data = strlen(valid_characters);
					}
					for (int i=0; i < *algorithm_data; i++) {
						if (current_char == valid_characters[i]) {
							fputc(current_char, output_file);
						}
					}
					break;
				default:
					if (current_char != -1) {
						fputc(current_char, output_file);
					}
					break;
			}
			if (algorithm_data) free(algorithm_data);
		}
		while (current_char != -1);
		if (write_and_exit) return 0;
	}
	fclose(input_file);
	fclose(output_file);
	first_cell = calloc(CELL_COUNT, CELL_SIZE);
	current_cell_pt = first_cell;
	code_window = NULL;
	output_window = NULL;
	flags.should_read_input = 0;
	cells_window = NULL;
	flags.did_resize = 0;
	if (!flags.no_curses) {
		output_buffer = malloc(OUTPUT_BUFFER_SIZE);
		output_buffer[0] = 0;
		pthread_create(&bf_thread, NULL, execute_bf_code, NULL);
		initscr();
		signal(SIGWINCH, handle_resize);
		redraw_screen();
	}
	else {
		execute_bf_code(NULL);
		return 0;
	}
	if (!flags.no_curses) {
		while (1) {
			if (flags.did_resize) {
				redraw_screen();
				flags.did_resize = 0;
			}
			pthread_mutex_lock(&instruction_pt_lock);
			char *code_pt = instruction_pt;
			pthread_mutex_unlock(&instruction_pt_lock);
			redraw_code_window(code_pt);
			redraw_cells_window();
			redraw_output_window();
			refresh_state_window();
			
			int input = wgetch(stdscr);
			char *kname = keyname(input);
			if (kname) {
				if (kname[0] == '^') switch (kname[1]) {
					case 'X': // Ctrl-X
						endwin();
						return 0;
					case 'T': // Ctrl-T
						flags.step_by_step = !flags.step_by_step;
						input = ERR;
						break;
					case '@': // Ctrl-Space
						if (flags.step_by_step) {
							flags.next_instruction = 1;
						}
						input = ERR;
						break;
				}
				else {
					switch (input) {
						case KEY_UP:
							if (delay <= MIN_DELAY) beep();
							else if ((delay / 2) % DELAY_INCREMENT) delay -= DELAY_INCREMENT;
							else delay /= 2;
							input = ERR;
							break;
						case KEY_DOWN:
							if (delay >= MAX_DELAY) beep();
							else delay = min(delay*2, MAX_DELAY);
							input = ERR;
					}
				}
				if ((input != 410) && flags.should_read_input && (input != ERR)) {
					*current_cell_pt = (cell_t)input;
					/*
					// This isn't right
					if (input == 127) {
						bf_putc('\b');
						bf_putc(' ');
						bf_putc('\b');
					}
					else */bf_putc(input);
					flags.should_read_input = 0;
					pthread_cond_signal(&input_cond);
				}
			}
			usleep(20000);
		}
	}
}
