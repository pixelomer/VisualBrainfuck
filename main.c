#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <ncurses.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>

typedef int cell_t;

#define min(a,b) ((a<b)?a:b)
#define max(a,b) ((a<b)?b:a)
#define CELL_COUNT 1024
#define CELL_SIZE sizeof(cell_t)
#define CELL_BUFFER_SIZE (CELL_COUNT * sizeof(cell_t))

// Make this value higher if you want the interpreter to be able to display more than 0x1000 characters.
// 0x1000 should be enough. When it is not enough, the oldest output will be deleted.
#define OUTPUT_BUFFER_SIZE 0x1000

static struct {
    unsigned int step_by_step:1;
	unsigned int did_resize:1;
	unsigned int next_instruction:1;
	unsigned int no_curses:1;
	unsigned int did_get_to_eof:1;
	unsigned int should_read_input:1;
} flags;
static cell_t *first_cell;
static cell_t *current_cell_pt;
static WINDOW *code_window;
static WINDOW *output_window;
static WINDOW *cells_window;
static WINDOW *input_window;
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
static char *last_drawn_code;

void handle_resize(int signal) {
	flags.did_resize = 1;
}

void *execute_bf_code(void *arg) {
#define return return NULL
	time_t start_time;
	int executed_instruction_count = 0;
	char input = 0;
	while ((input = *instruction_pt) != 0) {
		if (!executed_instruction_count) time(&start_time);
		if (delay) usleep(delay);
		if (!flags.did_get_to_eof && (!flags.step_by_step || flags.next_instruction)) {
			flags.next_instruction = 0;
			pthread_mutex_lock(&instruction_pt_lock);
			instruction_pt++;
			pthread_mutex_unlock(&instruction_pt_lock);
			char *tmp_instruction_pt = instruction_pt;
			switch (input) {
				case '<': {
					if (!(current_cell_pt - first_cell)) {
						current_cell_pt = first_cell + CELL_BUFFER_SIZE; // Simulate an overflow
					}
					else {
						current_cell_pt -= CELL_SIZE;
					}
					break;
				}
				case '>': {
					if ((current_cell_pt - first_cell) == CELL_BUFFER_SIZE) {
						current_cell_pt = first_cell; // Simulate an overflow
					}
					else {
						current_cell_pt += CELL_SIZE;
					}
					break;
				}
				case '+': {
					(*current_cell_pt)++;
					break;
				}
				case '-': {
					(*current_cell_pt)--;
					break;
				}
				case '.': {
					if (flags.no_curses) {
						printf("%c", *current_cell_pt);
					}
					else {
						pthread_mutex_lock(&output_lock);
						waddch(output_window, *current_cell_pt);
						long len = strlen(output_buffer);
						char *old_output_pt = output_buffer;
						if (len >= (OUTPUT_BUFFER_SIZE - 1)) old_output_pt++;
						sprintf(output_buffer, "%s%c", old_output_pt, (char)*current_cell_pt);
						pthread_mutex_unlock(&output_lock);
						wrefresh(output_window);
					}
					break;
				}
				case ',': {
					if (flags.no_curses) {
						cell_t user_input = (cell_t)fgetc(stdin);
						if (user_input != EOF) {
							*current_cell_pt = user_input;
							break;
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
				}
				case '[': {
					if (!*current_cell_pt) {
						long counter = 1;
						while (counter > 0) {
							if (tmp_instruction_pt >= (code+code_len-1)) return; // ERROR: Reached EOF before finding a matching bracket.
							input = (*(++tmp_instruction_pt));
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
				}
				case ']': {
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
#undef return
	return NULL;
}

void redraw_code_window(char *code_pt) {
	if (flags.no_curses) return;
	wclear(code_window);
	int max_x = getmaxx(stdscr);
	int cursor_pos = (max_x / 2);
	cursor_pos = cursor_pos + !((cursor_pos * 2) == max_x);
	mvwaddch(code_window, 1, (cursor_pos-1), '^');
	char *code_buffer = malloc(max_x + 1);
	mvwprintw(code_window, 0, 0, "%s", code_buffer);
	mvwprintw(code_window, 2, 0, "Current character: %ld", code_pt-code+1);
	if ((ips != -1) && !flags.step_by_step && !flags.did_get_to_eof) mvwprintw(code_window, 3, 0, "%d IPS", ips);
	wrefresh(code_window);
	free(code_buffer);
}

void redraw_cells_window(void) {
	if (flags.no_curses) return;
	wclear(cells_window);
	int max_y = min(getmaxy(cells_window), CELL_COUNT);
	int max_x = getmaxx(cells_window);
	const char *format = "#%d ==> %d";
	mvwprintw(cells_window, 0, 0, format, ((current_cell_pt-first_cell)/CELL_SIZE), *current_cell_pt);
	for (int i = 0; ((i < max_y-1) && (i < CELL_COUNT-1)); i++) {
		mvwprintw(cells_window, i+1, 0, format, i, *(first_cell+(i*CELL_SIZE)));
	}
	wrefresh(cells_window);
}

void redraw_screen(void) {
	if (flags.no_curses) return;
	endwin();
	refresh();
	clear();
	int screen_max_x, screen_max_y;
	getmaxyx(stdscr, screen_max_y, screen_max_x);
	int cells_window_w = (screen_max_x/2)-4;
	code_window = newwin(4, screen_max_x, 0, 0);
	int offset = !(((screen_max_x / 2) * 2) == screen_max_x);
	output_window = newwin(screen_max_y-8, cells_window_w+offset, 6, screen_max_x-cells_window_w-2-offset);
	cells_window = newwin(screen_max_y-8, cells_window_w, 6, 2);
	input_window = newwin(1, screen_max_x, screen_max_y-1, 0);
	int i;
	for (i = 6; i < screen_max_y-2; i++) {
		mvwaddch(stdscr, i, 0, '|');
		mvwaddch(stdscr, i, (screen_max_x/2), '|');
		mvwaddch(stdscr, i, screen_max_x-1, '|');
	}
	for (int j = 0; j < 2; j++) {
		int y = (5+(j*(i-5)));
		for (int k = 0; k < screen_max_x; k++) {
			mvaddch(y, k, '-');
		}
		mvwaddch(stdscr, y, 0, '+');
		mvwaddch(stdscr, y, (screen_max_x/2), '+');
		mvwaddch(stdscr, y, screen_max_x-1, '+');
	}
	wrefresh(stdscr);
	curs_set(0);
	nodelay(stdscr, 1);
	nodelay(input_window, 0);
	wtimeout(input_window, -1);
	keypad(stdscr, 1);
	noecho();
	scrollok(output_window, 1);
}

void redraw_output_window(void) {
	if (flags.no_curses) return;
	wclear(output_window);
	pthread_mutex_lock(&output_lock);
	mvwprintw(output_window, 0, 0, output_buffer);
	pthread_mutex_unlock(&output_lock);
	wrefresh(output_window);
}

void print_usage(_Bool should_exit) {
	fprintf(stderr,
		"Usage: %s [options] <program>\n"
		"  program: A file containing a brainfuck program or \"-\" to read from stdin.\n"
		"Options:\n"
		"  -n, --no-curses: Don't use ncurses. This will make the program simply execute the brainfuck program and exit.\n"
		"  -d, --debug-mode: Execute the program step by step. Use the space key to execute instructions. This can be toggled while running with the 't' key.\n"
		"  -h, --highest-speed: Start the program at the highest speed. The speed can be changed with the up and down arrow keys while running. Enabled by default with --no-curses.\n"
		"  -m, --minify: Minify the program and write it to the specified file.\n",
		program_path);
	if (should_exit) exit(1);
}

int main(int argc, char **argv) {
	code_len = 0;
	program_path = argv[0];
	delay = 10;
	ips = -1;
	int file_path_index = 0;

	// Parse the arguments
	FILE *minified_file = NULL;
	{
		static struct option long_opts[] = {
			{"highest-speed", no_argument, NULL, 'h'},
			{"debug-mode", no_argument, NULL, 'd'},
			{"no-curses", no_argument, NULL, 'n'},
			{"minify", required_argument, NULL, 'm'}
		};
		int option = 0;
		while ((option = getopt_long(argc, argv, "hdnm:", long_opts, NULL)) != -1) {
			switch (option) {
				case 'd':
					flags.step_by_step = 1;
					break;
				case 'n':
					flags.no_curses = 1;
				case 'h':
					delay = 0;
					break;
				case 'm':
					if (minified_file) {
						fprintf(stderr, "Error: You can specify the --minify option only once.\n");
						print_usage(1);
					}
					else if (!(minified_file = fopen(optarg ?: "", "w"))) {
						fprintf(stderr, "%s: %s: Failed to open file for writing\n", program_path, optarg);
						return 1;
					}
					break;
			}
		}
		// Do not expect proper code from me.
		if (flags.step_by_step && flags.no_curses) {
			fprintf(stderr, "Error: You can't use --debug-mode and --no-curses at the same time.\n");
			print_usage(1);
		}
		else if (optind != (file_path_index = (argc-1))) {
			print_usage(1);
		}
	}

	// Copy the program to memory
	{
		char *file_path = argv[file_path_index];
		FILE *input_file = (!strcmp(file_path, "-")) ? stdin : fopen(file_path, "r");
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
			const char *valid_characters = "[]-+<>,.";
			short len = strlen(valid_characters);
			for (short i = 0; i < len; i++) {
				if (valid_characters[i] == input) {
					if (written_bytes >= (buffer_size - 1)) {
						buffer_size *= 2;
						code = realloc(code, buffer_size);
					}
					code[written_bytes] = input;
					written_bytes++;
					if (minified_file) fputc(input, minified_file);
					break;
				}
			}
		}
		code[written_bytes] = 0;
		code = realloc(code, written_bytes+1);
		code_len = written_bytes;
		instruction_pt = code;
		fclose(input_file);
		if (minified_file) {
			fclose(minified_file);
			minified_file = NULL;
		}
	}
	first_cell = calloc(CELL_COUNT, CELL_SIZE);
	current_cell_pt = first_cell;
	code_window = NULL;
	output_window = NULL;
	flags.should_read_input = 1;
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

			int input = wgetch(stdscr);
			switch (input) {
				case 'Q':
				case 'q':
					endwin();
					return 0;
				case KEY_UP:
					if (delay <= 0) beep();
					else delay -= 10;
					break;
				case KEY_DOWN:
					if (delay >= 500000) beep();
					else delay += 10;
					break;
				case 'T':
				case 't':
					flags.step_by_step = !flags.step_by_step;
					break;
				case ' ':
					if (flags.step_by_step) {
						flags.next_instruction = 1;
					}
					break;
			}
			if (flags.should_read_input) {
				mvwprintw(input_window, 0, 0, "Program input: ");
				curs_set(1);
				wrefresh(input_window);
				while ((*current_cell_pt = (cell_t)wgetch(input_window)) == ERR);
				curs_set(0);
				wclear(input_window);
				wrefresh(input_window);
				flags.should_read_input = 0;
				pthread_cond_signal(&input_cond);
			}
			usleep(20000);
		}
	}
}
