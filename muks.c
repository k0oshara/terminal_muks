#include <ncurses.h>
#include <stdio.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#define INITIAL_CAPACITY 100
#define CMD_BUFFER_SIZE 256
#define STATUS_MSG_SIZE 256

typedef enum
{
    MODE_NORMAL,
    MODE_COMMAND,
    MODE_INSERT
} EditorMode;

typedef enum
{
    SYNTAX_NORMAL = 0,
    SYNTAX_KEYWORD,
    SYNTAX_STRING,
    SYNTAX_COMMENT,
    SYNTAX_NUMBER
} SyntaxType;

typedef struct
{
    char *text;
    int length;
    int *syntax;
} Line;

typedef struct
{
    Line **lines;
    int num_lines;
    int capacity;
} Buffer;

Buffer *gBuffer = NULL;

EditorMode gMode = MODE_NORMAL;
char gCmdBuffer[CMD_BUFFER_SIZE] = {0};
int gCmdLen = 0;
char gStatusMsg[STATUS_MSG_SIZE] = {0};

int gCursorX = 0;
int gCursorY = 0;
int gOffsetX = 0;
int gOffsetY = 0;
char *gFilename = NULL;
int gDirty = 0;

const char *c_keywords[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "define",
    "do", "doouble", "else", "enum", "extern", "float", "for", "goto", "if",
    "include", "inline", "int", "long", "register", "restrict", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "typedef", "typeof",
    "typeof_unqual", "union", "unsigned", "void", "volatile", "while", NULL};

Buffer *initBuffer(void)
{
    Buffer *buf = malloc(sizeof(Buffer));
    buf->lines = malloc(sizeof(Line *) * INITIAL_CAPACITY);
    buf->num_lines = 0;
    buf->capacity = INITIAL_CAPACITY;
    return buf;
}

void freeBuffer(Buffer *buf)
{
    for (int i = 0; i < buf->num_lines; i++)
    {
        free(buf->lines[i]->text);
        free(buf->lines[i]->syntax);
        free(buf->lines[i]);
    }
    free(buf->lines);
    free(buf);
}

void appendLine(Buffer *buf, const char *text)
{
    while (buf->num_lines >= buf->capacity)
    {
        const int new_capacity = buf->capacity * 2;
        Line **new_lines = realloc(buf->lines, sizeof(Line *) * new_capacity);
        if (!new_lines)
        {
            perror("Failed to reallocate memory for lines");
            freeBuffer(buf);
            exit(EXIT_FAILURE);
        }
        buf->capacity = new_capacity;
        buf->lines = new_lines;
    }
    Line *line = malloc(sizeof(Line));
    if (!line)
    {
        perror("Failed to allocate memory for new line");
        exit(EXIT_FAILURE);
    }

    line->length = text ? strlen(text) : 0;
    line->text = malloc(line->length + 1);
    if (!line->text)
    {
        perror("Failed to allocate memory for line text");
        free(line);
        exit(EXIT_FAILURE);
    }
    if (text && line->length > 0)
        memcpy(line->text, text, line->length);
    line->text[line->length] = '\0';

    line->syntax = malloc(line->length * sizeof(int));
    if (!line->syntax)
    {
        perror("Failed to allocate memory for syntax");
        free(line->text);
        free(line);
        exit(EXIT_FAILURE);
    }
    memset(line->syntax, SYNTAX_NORMAL, line->length * sizeof(int));

    buf->lines[buf->num_lines++] = line;
}

Line *getCurrentLine(void)
{
    while (gCursorY >= gBuffer->num_lines)
        appendLine(gBuffer, "");
    return gBuffer->lines[gCursorY];
}

void insertChar(Line *line, int pos, int ch)
{
    if (pos < 0 || pos > line->length)
        pos = line->length;

    char *new_text = realloc(line->text, line->length + 2);
    if (!new_text)
    {
        perror("Failed to allocate memory for line text");
        exit(EXIT_FAILURE);
    }
    line->text = new_text;

    memmove(line->text + pos + 1, line->text + pos, line->length - pos);
    line->text[pos] = (char)ch;
    line->length++;
    line->text[line->length] = '\0';

    int *new_syntax = realloc(line->syntax, line->length * sizeof(int));
    if (!new_syntax)
    {
        perror("Failed to reallocate syntax memory");
        exit(EXIT_FAILURE);
    }
    line->syntax = new_syntax;

    memmove(line->syntax + pos + 1, line->syntax + pos, (line->length - pos - 1) * sizeof(int));
    line->syntax[pos] = SYNTAX_NORMAL;

    gDirty = 1;
}

void deleteChar(Line *line, int pos)
{
    if (pos < 0 || pos >= line->length)
        return;
    memmove(&line->text[pos], &line->text[pos + 1], line->length - pos);
    line->length--;
    line->text[line->length] = '\0';
    gDirty = 1;
}

void update_line_syntax(Line *line)
{
    if (!line || !line->text || !line->syntax)
        return;
    int in_string = 0;             // ' " ' или ' ' '
    int in_multiline_comment = 0;  /* ... */
    int in_singleline_comment = 0; // //
    char current_word[32] = {0};
    int word_pos = 0;

    memset(line->syntax, SYNTAX_NORMAL, line->length * sizeof(int));

    for (int i = 0; i < line->length; i++)
    {
        char c = line->text[i];
        if (in_singleline_comment)
        {
            line->syntax[i] = SYNTAX_COMMENT;
            continue;
        }
        if (in_multiline_comment)
        {
            line->syntax[i] = SYNTAX_COMMENT;
            if (c == '*' && i < line->length - 1 && line->text[i + 1] == '/')
            {
                in_multiline_comment = 0;
                line->syntax[i + 1] = SYNTAX_COMMENT;
                i++;
            }
            continue;
        }
        if (in_string)
        {
            line->syntax[i] = SYNTAX_STRING;
            if (c == '\\' && i < line->length - 1)
            {
                line->syntax[i + 1] = SYNTAX_STRING;
                i++;
                continue;
            }
            if (c == in_string)
                in_string = 0;
            continue;
        }
        if (c == '"' || c == '\'')
        {
            in_string = c;
            line->syntax[i] = SYNTAX_STRING;
            continue;
        }

        if (c == '/' && i < line->length - 1 && line->text[i + 1] == '*')
        {
            line->syntax[i] = SYNTAX_COMMENT;
            line->syntax[i + 1] = SYNTAX_COMMENT;
            in_multiline_comment = 1;
            i++;
            continue;
        }
        if (c == '/' && i < line->length - 1 && line->text[i + 1] == '/')
        {
            memset(line->syntax + i, SYNTAX_COMMENT, (line->length - i) * sizeof(int));
            in_singleline_comment = 1;
            break;
        }

        if (isdigit(c) && (i == 0 || !(isalpha(line->text[i - 1]) || isdigit(line->text[i - 1]) || line->text[i - 1] == '_')))
        {
            int j = i;
            while (j < line->length && isdigit(line->text[j]))
                j++;

            if (j == line->length || !(isalpha(line->text[j]) || line->text[j] == '_'))
            {
                for (int k = i; k < j; k++)
                    line->syntax[k] = SYNTAX_NUMBER;
                i = j - 1;
                continue;
            }
        }
        if (isalpha(c) || c == '_')
        {
            if (word_pos < (int)sizeof(current_word) - 1)
                current_word[word_pos++] = c;
        }
        else
        {
            if (word_pos > 0)
            {
                current_word[word_pos] = '\0';
                for (int k = 0; c_keywords[k]; k++)
                {
                    if (strcmp(current_word, c_keywords[k]) == 0)
                    {
                        int start = i - word_pos;
                        for (int j = start; j < i; j++)
                            line->syntax[j] = SYNTAX_KEYWORD;
                        break;
                    }
                }
                word_pos = 0;
            }
        }
    }
    if (word_pos > 0)
    {
        current_word[word_pos] = '\0';
        for (int k = 0; c_keywords[k]; k++)
        {
            if (strcmp(current_word, c_keywords[k]) == 0)
            {
                int start = line->length - word_pos;
                for (int j = start; j < line->length; j++)
                    line->syntax[j] = SYNTAX_KEYWORD;
                break;
            }
        }
    }
}

int loadFile(Buffer *buf, const char *fname)
{
    FILE *fp = fopen(fname, "r");
    if (!fp)
        return -1;

    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    while ((read = getline(&line, &len, fp)) != -1)
    {
        if (read > 0 && (line[read - 1] == '\n' || line[read - 1] == '\r'))
            line[--read] = '\0';
        appendLine(buf, line);
    }
    free(line);
    fclose(fp);
    return 0;
}

int saveFile(Buffer *buf, const char *fname)
{
    FILE *fp = fopen(fname, "w");
    if (!fp)
        return -1;
    for (int i = 0; i < buf->num_lines; i++)
        fprintf(fp, "%s\n", buf->lines[i]->text);
    fclose(fp);
    return 0;
}

void ensureCursorInBounds(void)
{
    if (gCursorY < 0)
        gCursorY = 0;
    if (gCursorY >= gBuffer->num_lines)
        gCursorY = gBuffer->num_lines - 1;
    Line *line = gBuffer->lines[gCursorY];
    if (gCursorX < 0)
        gCursorX = 0;
    if (gCursorX > line->length)
        gCursorX = line->length;
}

void update_all_syntax()
{
    for (int i = 0; i < gBuffer->num_lines; i++)
    {
        update_line_syntax(gBuffer->lines[i]);
    }
}

void updateOffsets(void)
{
    int maxY, maxX;
    getmaxyx(stdscr, maxY, maxX);

    if (gCursorY < gOffsetY)
    {
        gOffsetY = gCursorY;
    }
    else if (gCursorY >= gOffsetY + (maxY - 2))
    {
        gOffsetY = gCursorY - (maxY - 2) + 1;
    }

    if (gCursorX < gOffsetX)
    {
        gOffsetX = gCursorX;
    }
    else if (gCursorX >= gOffsetX + maxX)
    {
        gOffsetX = gCursorX - maxX + 1;
    }
}

void drawScreen(void)
{
    erase();
    update_all_syntax();

    int maxY, maxX;
    getmaxyx(stdscr, maxY, maxX);

    for (int y = 0; y < maxY - 2; y++)
    {
        int lineNum = y + gOffsetY;
        if (lineNum >= gBuffer->num_lines)
            break;

        Line *line = gBuffer->lines[lineNum];
        for (int x = 0; x < maxX; x++)
        {
            int col = x + gOffsetX;
            if (col >= line->length)
                break;

            attrset(COLOR_PAIR(line->syntax[col]));
            mvaddch(y, x, line->text[col]);
        }
    }

    attron(A_REVERSE | COLOR_PAIR(1));

    char status[256];
    const char *mode = "";
    switch (gMode)
    {
    case MODE_NORMAL:
        mode = "NORMAL";
        break;
    case MODE_INSERT:
        mode = "INSERT";
        break;
    case MODE_COMMAND:
        mode = "COMMAND";
        break;
    }

    snprintf(status, sizeof(status), "%s | %s | %d,%d | %s",
             mode,
             gFilename ? gFilename : "[No FileName]",
             gCursorY + 1, gCursorX + 1,
             gDirty ? "[+]" : "");

    int len = strlen(status);
    if (len > maxX)
        len = maxX;
    mvprintw(maxY - 2, 0, "%.*s", len, status);
    mvprintw(maxY - 1, 0, "%s", gStatusMsg);
    clrtoeol();

    if (gMode == MODE_COMMAND)
    {
        attroff(A_REVERSE);
        mvprintw(maxY - 1, 0, "%s", gCmdBuffer);
        clrtoeol();
    }

    int cursorScreenY = gCursorY - gOffsetY;
    int cursorScreenX = gCursorX - gOffsetX;
    if (cursorScreenY >= 0 && cursorScreenY < maxY - 2)
    {
        move(cursorScreenY, cursorScreenX);
    }

    refresh();
}

void processNormalMode(int ch)
{
    switch (ch)
    {
    case 'h':
    case KEY_LEFT:
        gCursorX--;
        break;
    case 'j':
    case KEY_DOWN:
        gCursorY++;
        break;
    case 'k':
    case KEY_UP:
        gCursorY--;
        break;
    case 'l':
    case KEY_RIGHT:
        gCursorX++;
        break;

    case 'i':
        gMode = MODE_INSERT;
        break;
    case ':':
        gMode = MODE_COMMAND;
        memset(gCmdBuffer, 0, CMD_BUFFER_SIZE);
        gCmdLen = 0;
        gCmdBuffer[gCmdLen++] = ':';
        break;
    case '1':
        gCursorX = 0;
        break;
    case '2':
        gCursorX = gBuffer->lines[gCursorY]->length;
        break;
    default:
        break;
    }

    ensureCursorInBounds();
}

void processInsertMode(int ch)
{
    switch (ch)
    {
    case 27: // ESC
        gMode = MODE_NORMAL;
        break;
    case KEY_BACKSPACE:
        if (gCursorX > 0)
        {
            deleteChar(gBuffer->lines[gCursorY], gCursorX - 1);
            gCursorX--;
        }
        break;
    case KEY_UP:
        if (gCursorY > 0)
            gCursorY--;
        break;
    case KEY_DOWN:
        if (gCursorY < gBuffer->num_lines - 1)
            gCursorY++;
        else
            appendLine(gBuffer, "");
        break;
    case KEY_LEFT:
        if (gCursorX > 0)
            gCursorX--;
        break;
    case KEY_RIGHT:
        if (gCursorX < gBuffer->lines[gCursorY]->length)
            gCursorX++;
        break;
    default:
        insertChar(gBuffer->lines[gCursorY], gCursorX, ch);
        gCursorX++;
        break;
    }

    ensureCursorInBounds();
}

void editorSetStatusMessage(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gStatusMsg, STATUS_MSG_SIZE, fmt, ap);
    va_end(ap);
}

void processCommandMode(int ch)
{
    switch (ch)
    {
    case '\n':
        if (strcmp(gCmdBuffer, ":wq") == 0)
        {
            if (gFilename != NULL)
            {
                if (saveFile(gBuffer, gFilename) == 0)
                {
                    gDirty = 0;
                    gMode = MODE_NORMAL;
                    editorSetStatusMessage("File saved and exiting!");
                    endwin();
                    exit(0);
                }
                else
                {
                    editorSetStatusMessage("Error saving file!");
                }
            }
            gMode = MODE_NORMAL;
        }
        else if (strcmp(gCmdBuffer, ":w") == 0)
        {
            if (gFilename != NULL)
            {
                if (saveFile(gBuffer, gFilename) == 0)
                {
                    gDirty = 0;
                    gMode = MODE_NORMAL;
                    editorSetStatusMessage("File saved!");
                }
                else
                {
                    editorSetStatusMessage("Error saving file!");
                }
            }
            else
            {
                editorSetStatusMessage("No filename to save.");
            }
            gMode = MODE_NORMAL;
        }
        else if (strcmp(gCmdBuffer, ":q!") == 0)
        {
            gMode = MODE_NORMAL;
            editorSetStatusMessage("Exiting without saving.");
            endwin();
            exit(0);
        }
        else if (strcmp(gCmdBuffer, ":q") == 0)
        {
            if (gDirty)
            {
                editorSetStatusMessage("You have unsaved changes. Use :w to save.");
            }
            else
            {
                gMode = MODE_NORMAL;
                endwin();
                freeBuffer(gBuffer);
                if (gFilename)
                    free(gFilename);
                exit(EXIT_SUCCESS);
            }
            gMode = MODE_NORMAL;
        }
        else
        {
            editorSetStatusMessage("Unknown command!");
            gMode = MODE_NORMAL;
        }
        break;

    case 27: // ESC
        gMode = MODE_NORMAL;
        break;
    case KEY_BACKSPACE:
        if (gCmdLen > 0)
        {
            gCmdLen--;
            gCmdBuffer[gCmdLen] = '\0';
        }
        break;
    default:
        if (gCmdLen < CMD_BUFFER_SIZE - 1)
        {
            gCmdBuffer[gCmdLen++] = ch;
            gCmdBuffer[gCmdLen] = '\0';
        }
        break;
    }
}

int main(int argc, char *argv[])
{
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    gBuffer = initBuffer();
    if (argc >= 2)
    {
        gFilename = strdup(argv[1]);
        if (!gFilename)
        {
            perror("Failed to allocate filename");
            exit(EXIT_FAILURE);
        }
        if (loadFile(gBuffer, gFilename) == -1)
        {
            editorSetStatusMessage("Failed to open: %s", argv[1]);
        }
    }
    else
    {
        appendLine(gBuffer, "");
    }
    gDirty = 0;
    gCursorX = 0;
    gCursorY = 0;
    gOffsetX = 0;
    gOffsetY = 0;

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    if (has_colors())
    {
        start_color();
        init_pair(SYNTAX_NORMAL, COLOR_WHITE, COLOR_BLACK);   // normal text
        init_pair(SYNTAX_KEYWORD, COLOR_YELLOW, COLOR_BLACK); // keywords
        init_pair(SYNTAX_STRING, COLOR_GREEN, COLOR_BLACK);   // string literals
        init_pair(SYNTAX_COMMENT, COLOR_CYAN, COLOR_BLACK);   // comments
        init_pair(SYNTAX_NUMBER, COLOR_MAGENTA, COLOR_BLACK); // numbers
    }

    while (1)
    {
        updateOffsets();
        drawScreen();
        int ch = getch();
        switch (gMode)
        {
        case MODE_NORMAL:
            processNormalMode(ch);
            break;
        case MODE_INSERT:
            processInsertMode(ch);
            break;
        case MODE_COMMAND:
            processCommandMode(ch);
            break;
        }
        update_all_syntax();
    }

    endwin();
    freeBuffer(gBuffer);
    if (gFilename)
        free(gFilename);
    return 0;
}
