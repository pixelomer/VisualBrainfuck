#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <ncurses.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <getopt.h>

typedef int cell_t;

#define min(a,b) ((a<b)?a:b)
#define max(a,b) ((a<b)?b:a)
#define CELL_COUNT 1024
#define CELL_SIZE sizeof(cell_t)
#define CELL_BUFFER_SIZE (CELL_COUNT * sizeof(cell_t))

// Make this value higher if your screen is really big and has more than 0x400 columns. Highly unlikely.
#define CODE_BUFFER_SIZE 0x400

// Make this value higher if you want the interpreter to be able to display more than 0x1000 characters.
// 0x1000 should be enough. When it is not enough, the oldest output will be deleted.
#define OUTPUT_BUFFER_SIZE 0x1000

static struct {
    unsigned int step_by_step:1;
	unsigned int did_resize:1;
	unsigned int next_instruction:1;
	unsigned int no_curses:1;
} options;
static cell_t *first_cell;
static cell_t *current_cell_pt;
static WINDOW *code_window;
static WINDOW *output_window;
static WINDOW *cells_window;
static WINDOW *input_window;
static char *code_buffer;
static char *output_buffer;
static FILE *file;
static useconds_t delay;
static int ips;
static char *program_path;

void handle_resize(int signal) {
	options.did_resize = 1;
}

void reload_options(void) {
	
}

void redraw_code_window(void) {
	if (options.no_curses) return;
	wclear(code_window);
	int max_x = min(getmaxx(stdscr), CODE_BUFFER_SIZE);
	int cursor_pos = (max_x / 2);
	cursor_pos = cursor_pos + !((cursor_pos * 2) == max_x);
	mvwaddch(code_window, 1, (cursor_pos-1), '^');
	long current_char_index = ftell(file);
	long offset = 0-min(cursor_pos, current_char_index);
	fseek(file, offset, SEEK_CUR);
	for (int i = 0; i < max_x; i++) code_buffer[i] = ' ';
	long read = fread((code_buffer+(offset+cursor_pos)), 1, max_x-offset, file);
	fseek(file, 0-read-offset, SEEK_CUR);
	code_buffer[max_x-1] = 0;
	mvwprintw(code_window, 0, 0, "%s", code_buffer);
	mvwprintw(code_window, 2, 0, "Current character: %ld", ftell(file));
	if ((ips != -1) && !options.step_by_step) mvwprintw(code_window, 3, 0, "%d IPS", ips);
	wrefresh(code_window);
}

void redraw_cells_window(void) {
	if (options.no_curses) return;
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
	if (options.no_curses) return;
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
	reload_options();
	nodelay(stdscr, 1);
	nodelay(input_window, 0);
	wtimeout(input_window, -1);
	keypad(stdscr, 1);
	noecho();
	scrollok(output_window, 1);
}

void redraw_output_window(void) {
	if (options.no_curses) return;
	wclear(output_window);
	mvwprintw(output_window, 0, 0, output_buffer);
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
	program_path = argv[0];
	delay = 5000;
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
					options.step_by_step = 1;
					break;
				case 'n':
					options.no_curses = 1;
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
		if (options.step_by_step && options.no_curses) {
			fprintf(stderr, "Error: You can't use --debug-mode and --no-curses at the same time.\n");
			print_usage(1);
		}
		else if (optind != (file_path_index = (argc-1))) {
			print_usage(1);
		}
	}

	// Create a temporary file that only contains the brainfuck characters using the input
	file = NULL;
	{
		char *file_path = argv[file_path_index];
		FILE *input_file = (!strcmp(file_path, "-")) ? stdin : fopen(file_path, "r");
		if (!input_file) {
			fprintf(stderr, "%s: %s: Failed to open file for reading\n", argv[0], file_path);
			return 2;
		}
		file = tmpfile();
		if (!file) {
			fprintf(stderr, "%s: Failed to create a temporary file for the program\n", argv[0]);
			return 2;
		}
		while (1) {
			char input = fgetc(input_file);
			if (input == EOF) break;
			const char *valid_characters = "[]-+<>,.";
			short len = strlen(valid_characters);
			for (short i = 0; i < len; i++) {
				if (valid_characters[i] == input) {
					fputc(input, file);
					if (minified_file) fputc(input, minified_file);
					break;
				}
			}
		}
		rewind(file);
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
	cells_window = NULL;
	options.did_resize = 0;
	if (!options.no_curses) {
		initscr();
		code_buffer = malloc(CODE_BUFFER_SIZE);
		output_buffer = malloc(OUTPUT_BUFFER_SIZE);
		output_buffer[0] = 0;
		signal(SIGWINCH, handle_resize);
		redraw_screen();
	}
	_Bool did_get_to_eof = 0;
	time_t start_time;
	int executed_instruction_count = 0;
	while (1) {
		if (!executed_instruction_count) time(&start_time);
		if (delay) usleep(delay);
		// Read the new character
		if (!did_get_to_eof && (!options.step_by_step || options.next_instruction)) {
			options.next_instruction = 0;
			char input = (char)fgetc(file);
			if (input == EOF) {
				did_get_to_eof = 1;
				continue;
			}
			
			// Handle the new character
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
					if (options.no_curses) {
						printf("%c", *current_cell_pt);
					}
					else {
						waddch(output_window, *current_cell_pt);
						long len = strlen(output_buffer);
						char *old_output_pt = output_buffer;
						if (len >= (OUTPUT_BUFFER_SIZE - 1)) old_output_pt++;
						sprintf(output_buffer, "%s%c", old_output_pt, (char)*current_cell_pt);
						wrefresh(output_window);
					}
					break;
				}
				case ',': {
					if (options.no_curses) {
						if ((*current_cell_pt = (cell_t)fgetc(stdin)) == EOF) {
							return 0;
						}
					}
					else {
						mvwprintw(input_window, 0, 0, "Program input: ");
						curs_set(1);
						wrefresh(input_window);
						if ((*current_cell_pt = (cell_t)wgetch(input_window)) == ERR) {
							*current_cell_pt = -1;
						}
						curs_set(0);
						wclear(input_window);
						wrefresh(input_window);
					}
					break;
				}
				case '[': {
					if (!*current_cell_pt) {
						long counter = 1;
						while (counter > 0) {
							input = (char)fgetc(file);
							if (input == EOF) return 3;
							switch (input) {
								case EOF: return 3;
								case '[': counter++; break;
								case ']': counter--; break;
							}
						}
					}
					break;
				}
				case ']': {
					if (*current_cell_pt) {
						long counter = 1;
						while (counter > 0) {
							if (fseek(file, -2, SEEK_CUR) == -1) return 4;
							input = (char)fgetc(file);
							switch (input) {
								case EOF: return 4;
								case '[': counter--; break;
								case ']': counter++; break;
							}
						}
						fseek(file, -1, SEEK_CUR);
					}
					break;
				}
			}
		}
		else if (options.no_curses) {
			return 0;
		}

		if (!options.no_curses) {
			if (options.did_resize) {
				redraw_screen();
				options.did_resize = 0;
			}
			redraw_code_window();
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
					else delay -= 5000;
					break;
				case KEY_DOWN:
					if (delay >= 500000) beep();
					else delay += 5000;
					break;
				case 'T':
				case 't':
					options.step_by_step = !options.step_by_step;
					reload_options();
					break;
				case ' ':
					if (options.step_by_step) {
						options.next_instruction = 1;
					}
					break;
			}
		}

		executed_instruction_count++;

		if ((time(NULL) - start_time) >= 1) {
			ips = executed_instruction_count;
			executed_instruction_count = 0;
		}
	}
}