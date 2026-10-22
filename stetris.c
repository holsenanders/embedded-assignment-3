#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <fcntl.h>
#include <linux//fb.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <poll.h>
#include <stdint.h>
// The game state can be used to detect what happens on the playfield
#define GAMEOVER 0
#define ACTIVE (1 << 0)
#define ROW_CLEAR (1 << 1)
#define TILE_ADDED (1 << 2)
#define PAUSED (1 << 3)

// If you extend this structure, either avoid pointers or adjust
// the game logic allocate/deallocate and reset the memory
typedef struct
{
    bool occupied;
    uint16_t color;
} tile;

typedef struct
{
    unsigned int x;
    unsigned int y;
} coord;

typedef struct
{
    coord const grid;                     // playfield bounds
    unsigned long const uSecTickTime;     // tick rate
    unsigned long const rowsPerLevel;     // speed up after clearing rows
    unsigned long const initNextGameTick; // initial value of nextGameTick

    unsigned int tiles; // number of tiles played
    unsigned int rows;  // number of rows cleared
    unsigned int score; // game score
    unsigned int level; // game level

    tile *rawPlayfield; // pointer to raw memory of the playfield
    tile **playfield;   // This is the play field array
    unsigned int state;
    coord activeTile; // current tile

    unsigned long tick;         // incremeted at tickrate, wraps at nextGameTick
                                // when reached 0, next game state calculated
    unsigned long nextGameTick; // sets when tick is wrapping back to zero
                                // lowers with increasing level, never reaches 0
} gameConfig;

gameConfig game = {
    .grid = {8, 8},
    .uSecTickTime = 10000,
    .rowsPerLevel = 2,
    .initNextGameTick = 50,
};


// current color;
int current_color = 0;

// Game colors found through a color picker
uint16_t red = ((31 << 11) | (0 << 5) | 0);
uint16_t blue = ((0 << 11) | (0 << 5) | 31);
uint16_t green = ((0 << 11) | (63 << 5) | 0);
uint16_t pink = ((31 << 11) | (0 << 5) | 25);
uint16_t yellow = ((30 << 11) | (63 << 5) | 0);

//holder for the pi sense frame buffer id
char pi_sense_id[] = "RPi-Sense FB";
struct fb_fix_screeninfo sense_hat_fb_info;
int sense_hat_frame_buffer_file_descriptor = -1;
int frame_buffer_size;
uint16_t *sense_hat_frame_buffer;

//Joystick global vars
char joystick_id[] = "Raspberry Pi Sense HAT Joystick";
int sense_hat_joystick_file_descriptor = -1;

// This function is called on the start of your application
// Here you can initialize what ever you need for your task
// return false if something fails, else true
bool initializeSenseHat() {
    struct fb_fix_screeninfo scoped_fb_info;

    // Find the pi sense hat frame buffer
    for (int i = 0; i < 32; i++){
        char path_to_frame_buffer[32];
        // concat the path to the possible frame buffer
        snprintf(path_to_frame_buffer, sizeof(path_to_frame_buffer), "/dev/fb%d", i);

        // open returns an int representing success or not, we need to read and write to this device
        int file_descriptor_to_check_frame_buffer = open(path_to_frame_buffer, O_RDWR);

        if (file_descriptor_to_check_frame_buffer == -1) {
            close(file_descriptor_to_check_frame_buffer);
            continue;
        }

        // Get the info for the driver and put it in the address of scoped_fb_info
        ioctl(file_descriptor_to_check_frame_buffer, FBIOGET_FSCREENINFO, &scoped_fb_info);

        if (strcmp(scoped_fb_info.id, pi_sense_id) == 0) {
            sense_hat_fb_info = scoped_fb_info;
            sense_hat_frame_buffer_file_descriptor = file_descriptor_to_check_frame_buffer;
            frame_buffer_size = sense_hat_fb_info.smem_len;

            // Map from virtual to physical address
            sense_hat_frame_buffer = mmap(
                NULL,
                frame_buffer_size,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                sense_hat_frame_buffer_file_descriptor,
                0);

            // If sense hat map fail we free the buffer and close the file descriptor
            if (sense_hat_frame_buffer == MAP_FAILED) {
                sense_hat_frame_buffer = NULL;
                close(file_descriptor_to_check_frame_buffer);
                sense_hat_frame_buffer_file_descriptor = -1;
                return false;
            }
            // If mapping is successful we break.
            break;
        }
        // close the file descriptors that are non-matching
        close(file_descriptor_to_check_frame_buffer);
    }
    // If the frame buffer is a null pointer we return early.
    if (sense_hat_frame_buffer ==  NULL) {
        return false;
    }

    for (int i = 0; i < 32; i++) {
        char path_to_joystick[32];
        snprintf(path_to_joystick, sizeof(path_to_joystick), "/dev/input/event%d", i);

        int file_descriptor_to_check_joystick = open(path_to_joystick, O_RDONLY | O_NONBLOCK);

        if (file_descriptor_to_check_joystick == -1) {
            close(file_descriptor_to_check_joystick);
            continue;
        }

        char input_device_name_to_check[256] = "Unknown";
        if (ioctl(file_descriptor_to_check_joystick, EVIOCGNAME(sizeof(input_device_name_to_check)), input_device_name_to_check) != -1
            && strcmp(input_device_name_to_check, joystick_id) == 0) {
            sense_hat_joystick_file_descriptor = file_descriptor_to_check_joystick;
            return true;
        }
        close(file_descriptor_to_check_joystick);
    }
    return false;
}

// This function is called when the application exits
// Here you can free up everything that you might have opened/allocated
void freeSenseHat()
{
    // turn off all LEDs
    if (sense_hat_frame_buffer) {
        memset(sense_hat_frame_buffer, 0, frame_buffer_size);
        munmap(sense_hat_frame_buffer, frame_buffer_size);
    }

    // Close the file descriptor if open returned anything other than -1 indicating an error
    if (sense_hat_frame_buffer_file_descriptor != -1) {
        close(sense_hat_frame_buffer_file_descriptor);
    }

    // Close the joystick file descriptor in the way as the frame buffer file descriptor
    if (sense_hat_joystick_file_descriptor != -1) {
        close(sense_hat_joystick_file_descriptor);
    }

}

// This function should return the key that corresponds to the joystick press
// KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, with the respective direction
// and KEY_ENTER, when the joystick is pressed
// KEY_UP toggles pause (handled by the game logic)
// !!! when nothing was pressed you MUST return 0 !!!
int readSenseHatJoystick()
{
    struct input_event event_from_joystick;
    while (read(sense_hat_joystick_file_descriptor, &event_from_joystick, sizeof(event_from_joystick)) == sizeof(event_from_joystick)) {
        if (event_from_joystick.type == EV_KEY) {
            // Value == 1 is the initial press down
            if (event_from_joystick.value == 1) {
                return event_from_joystick.code;
            }
            // Value 2 is holding the button so we exclude KEY_UP which pauses the game
            if (event_from_joystick.value == 2 && event_from_joystick.code != KEY_UP) {
                return  event_from_joystick.code;
            }
        }
    }
    return 0;
}

// This function should render the gamefield on the LED matrix. It is called
// every game tick. The parameter playfieldChanged signals whether the game logic
// has changed the playfield
void renderSenseHatMatrix(bool const playfieldChanged)
{
    if (!playfieldChanged)
        return;

    for (int y = 0; y < game.grid.y; y++) {
        for (int x = 0; x < game.grid.x; x++) {
            tile const t = game.playfield[y][x];
            // Since frame buffer is flat we calculate the x and y coords by multiplying the y coord with the with and
            // add x as offset. We set the color defined in struct if occupied, 0 if not.
            sense_hat_frame_buffer[y * game.grid.x + x] = t.occupied ? t.color : 0;
        }
    }
}

// The game logic uses only the following functions to interact with the playfield.
// if you choose to change the playfield or the tile structure, you might need to
// adjust this game logic <> playfield interface

static inline void newTile(coord const target)
{
    game.playfield[target.y][target.x].occupied = true;

    switch (current_color) {
        case 0:
            game.playfield[target.y][target.x].color = red;
            current_color++;
            break;
        case 1:
            game.playfield[target.y][target.x].color = blue;
            current_color++;
            break;
        case 2:
            game.playfield[target.y][target.x].color = green;
            current_color++;
            break;
        case 3:
            game.playfield[target.y][target.x].color = pink;
            current_color++;
            break;
        case 4:
            game.playfield[target.y][target.x].color = yellow;
            current_color = 0;
            break;
    }
}

static inline void copyTile(coord const to, coord const from)
{
    memcpy((void *)&game.playfield[to.y][to.x], (void *)&game.playfield[from.y][from.x], sizeof(tile));
}

static inline void copyRow(unsigned int const to, unsigned int const from)
{
    memcpy((void *)&game.playfield[to][0], (void *)&game.playfield[from][0], sizeof(tile) * game.grid.x);
}

static inline void resetTile(coord const target)
{
    memset((void *)&game.playfield[target.y][target.x], 0, sizeof(tile));
}

static inline void resetRow(unsigned int const target)
{
    memset((void *)&game.playfield[target][0], 0, sizeof(tile) * game.grid.x);
}

static inline bool tileOccupied(coord const target)
{
    return game.playfield[target.y][target.x].occupied;
}

static inline bool rowOccupied(unsigned int const target)
{
    for (unsigned int x = 0; x < game.grid.x; x++)
    {
        coord const checkTile = {x, target};
        if (!tileOccupied(checkTile))
        {
            return false;
        }
    }
    return true;
}

static inline void resetPlayfield()
{
    for (unsigned int y = 0; y < game.grid.y; y++)
    {
        resetRow(y);
    }
}

// Below here comes the game logic. Keep in mind: You are not allowed to change how the game works!
// that means no changes are necessary below this line! And if you choose to change something
// keep it compatible with what was provided to you!

bool addNewTile()
{
    game.activeTile.y = 0;
    game.activeTile.x = (game.grid.x - 1) / 2;
    if (tileOccupied(game.activeTile))
        return false;
    newTile(game.activeTile);
    return true;
}

bool moveRight()
{
    coord const newTile = {game.activeTile.x + 1, game.activeTile.y};
    if (game.activeTile.x < (game.grid.x - 1) && !tileOccupied(newTile))
    {
        copyTile(newTile, game.activeTile);
        resetTile(game.activeTile);
        game.activeTile = newTile;
        return true;
    }
    return false;
}

bool moveLeft()
{
    coord const newTile = {game.activeTile.x - 1, game.activeTile.y};
    if (game.activeTile.x > 0 && !tileOccupied(newTile))
    {
        copyTile(newTile, game.activeTile);
        resetTile(game.activeTile);
        game.activeTile = newTile;
        return true;
    }
    return false;
}

bool moveDown()
{
    coord const newTile = {game.activeTile.x, game.activeTile.y + 1};
    if (game.activeTile.y < (game.grid.y - 1) && !tileOccupied(newTile))
    {
        copyTile(newTile, game.activeTile);
        resetTile(game.activeTile);
        game.activeTile = newTile;
        return true;
    }
    return false;
}

bool clearRow()
{
    if (rowOccupied(game.grid.y - 1))
    {
        for (unsigned int y = game.grid.y - 1; y > 0; y--)
        {
            copyRow(y, y - 1);
        }
        resetRow(0);
        return true;
    }
    return false;
}

void advanceLevel()
{
    game.level++;
    switch (game.nextGameTick)
    {
    case 1:
        break;
    case 2 ... 10:
        game.nextGameTick--;
        break;
    case 11 ... 20:
        game.nextGameTick -= 2;
        break;
    default:
        game.nextGameTick -= 10;
    }
}

void newGame()
{
    game.state = ACTIVE;
    game.tiles = 0;
    game.rows = 0;
    game.score = 0;
    game.tick = 0;
    game.level = 0;
    resetPlayfield();
}

void gameOver()
{
    game.state = GAMEOVER;
    game.nextGameTick = game.initNextGameTick;
}

bool sTetris(int const key)
{
    bool playfieldChanged = false;

    if (game.state & ACTIVE)
    {
        // Toggle pause on KEY_UP (Sense Hat joystick up)
        if (key == KEY_UP)
        {
            game.state ^= PAUSED;
            return false;
        }

        // While paused, freeze the playfield (no moves, no gravity)
        if (game.state & PAUSED)
        {
            return false;
        }

        // Move the current tile
        if (key)
        {
            playfieldChanged = true;
            switch (key)
            {
            case KEY_LEFT:
                moveLeft();
                break;
            case KEY_RIGHT:
                moveRight();
                break;
            case KEY_DOWN:
                while (moveDown())
                {
                };
                game.tick = 0;
                break;
            default:
                playfieldChanged = false;
            }
        }

        // If we have reached a tick to update the game
        if (game.tick == 0)
        {
            // We communicate the row clear and tile add over the game state
            // clear these bits if they were set before
            game.state &= ~(ROW_CLEAR | TILE_ADDED);

            playfieldChanged = true;
            // Clear row if possible
            if (clearRow())
            {
                game.state |= ROW_CLEAR;
                game.rows++;
                game.score += game.level + 1;
                if ((game.rows % game.rowsPerLevel) == 0)
                {
                    advanceLevel();
                }
            }

            // if there is no current tile or we cannot move it down,
            // add a new one. If not possible, game over.
            if (!tileOccupied(game.activeTile) || !moveDown())
            {
                if (addNewTile())
                {
                    game.state |= TILE_ADDED;
                    game.tiles++;
                }
                else
                {
                    gameOver();
                }
            }
        }
    }

    // Press any key to start a new game
    if ((game.state == GAMEOVER) && key)
    {
        playfieldChanged = true;
        newGame();
        addNewTile();
        game.state |= TILE_ADDED;
        game.tiles++;
    }

    return playfieldChanged;
}

int readKeyboard()
{
    struct pollfd pollStdin = {
        .fd = STDIN_FILENO,
        .events = POLLIN};
    int lkey = 0;

    if (poll(&pollStdin, 1, 0))
    {
        lkey = fgetc(stdin);
        if (lkey != 27)
            goto exit;
        lkey = fgetc(stdin);
        if (lkey != 91)
            goto exit;
        lkey = fgetc(stdin);
    }
exit:
    switch (lkey)
    {
    case 10:
        return KEY_ENTER;
    case 65:
        return KEY_UP;
    case 66:
        return KEY_DOWN;
    case 67:
        return KEY_RIGHT;
    case 68:
        return KEY_LEFT;
    }
    return 0;
}

void renderConsole(bool const playfieldChanged)
{
    if (!playfieldChanged)
        return;

    // Goto beginning of console
    fprintf(stdout, "\033[%d;%dH", 0, 0);
    for (unsigned int x = 0; x < game.grid.x + 2; x++)
    {
        fprintf(stdout, "-");
    }
    fprintf(stdout, "\n");
    for (unsigned int y = 0; y < game.grid.y; y++)
    {
        fprintf(stdout, "|");
        for (unsigned int x = 0; x < game.grid.x; x++)
        {
            coord const checkTile = {x, y};
            fprintf(stdout, "%c", (tileOccupied(checkTile)) ? '#' : ' ');
        }
        switch (y)
        {
        case 0:
            fprintf(stdout, "| Tiles: %10u\n", game.tiles);
            break;
        case 1:
            fprintf(stdout, "| Rows:  %10u\n", game.rows);
            break;
        case 2:
            fprintf(stdout, "| Score: %10u\n", game.score);
            break;
        case 4:
            fprintf(stdout, "| Level: %10u\n", game.level);
            break;
        case 7:
            fprintf(stdout, "| %17s\n", (game.state == GAMEOVER) ? "Game Over" : "");
            break;
        default:
            fprintf(stdout, "|\n");
        }
    }
    for (unsigned int x = 0; x < game.grid.x + 2; x++)
    {
        fprintf(stdout, "-");
    }
    fflush(stdout);
}

inline unsigned long uSecFromTimespec(struct timespec const ts)
{
    return ((ts.tv_sec * 1000000) + (ts.tv_nsec / 1000));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    // This sets the stdin in a special state where each
    // keyboard press is directly flushed to the stdin and additionally
    // not outputted to the stdout
    {
        struct termios ttystate;
        tcgetattr(STDIN_FILENO, &ttystate);
        ttystate.c_lflag &= ~(ICANON | ECHO);
        ttystate.c_cc[VMIN] = 1;
        tcsetattr(STDIN_FILENO, TCSANOW, &ttystate);
    }

    // Allocate the playing field structure
    game.rawPlayfield = (tile *)malloc(game.grid.x * game.grid.y * sizeof(tile));
    game.playfield = (tile **)malloc(game.grid.y * sizeof(tile *));
    if (!game.playfield || !game.rawPlayfield)
    {
        fprintf(stderr, "ERROR: could not allocate playfield\n");
        return 1;
    }
    for (unsigned int y = 0; y < game.grid.y; y++)
    {
        game.playfield[y] = &(game.rawPlayfield[y * game.grid.x]);
    }

    // Reset playfield to make it empty
    resetPlayfield();
    // Start with gameOver
    gameOver();

    if (!initializeSenseHat())
    {
        fprintf(stderr, "ERROR: could not initilize sense hat\n");
        return 1;
    };

    // Clear console, render first time
    fprintf(stdout, "\033[H\033[J");
    renderConsole(true);
    renderSenseHatMatrix(true);

    while (true)
    {
        struct timeval sTv, eTv;
        gettimeofday(&sTv, NULL);

        int key = readSenseHatJoystick();
        if (!key)
        {
            // NOTE: Uncomment the next line if you want to test your implementation with
            // reading the inputs from stdin. However, we expect you to read the inputs directly
            // from the input device and not from stdin (you should implement the readSenseHatJoystick
            // method).
            //key = readKeyboard();
        }
        if (key == KEY_ENTER)
            break;

        bool playfieldChanged = sTetris(key);
        renderConsole(playfieldChanged);
        renderSenseHatMatrix(playfieldChanged);

        // Wait for next tick
        gettimeofday(&eTv, NULL);
        unsigned long const uSecProcessTime = ((eTv.tv_sec * 1000000) + eTv.tv_usec) - ((sTv.tv_sec * 1000000 + sTv.tv_usec));
        if (uSecProcessTime < game.uSecTickTime)
        {
            usleep(game.uSecTickTime - uSecProcessTime);
        }
        game.tick = (game.tick + 1) % game.nextGameTick;
    }

    freeSenseHat();
    free(game.playfield);
    free(game.rawPlayfield);

    return 0;
}
