// grind.cpp : Library Management System - interactive console client for testdb.
//

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cmath>
#include <conio.h>   // _getch, for hidden password input
#include <io.h>      // _isatty, _fileno
#include <mysqlx/xdevapi.h>
#include <sqlite3.h>

// ---------------------------------------------------------------------------
// Input helpers
// ---------------------------------------------------------------------------

// Read a full line (names can contain spaces, so avoid `std::cin >>`).
std::string prompt(const std::string& label)
{
    std::cout << label;
    std::string line;
    if (!std::getline(std::cin, line))
        return "";   // EOF (e.g. Ctrl+Z) -- callers treat empty as "cancel"
    return line;
}

// Read a password without echoing it. Falls back to a normal line read when
// stdin isn't a console (e.g. input piped in from a file).
std::string promptPassword(const std::string& label)
{
    if (!_isatty(_fileno(stdin)))
        return prompt(label);

    std::cout << label;
    std::string pw;
    int ch;
    while ((ch = _getch()) != '\r') {
        if (ch == '\b') {
            if (!pw.empty()) {
                pw.pop_back();
                std::cout << "\b \b";
            }
        }
        else if (ch == 0 || ch == 0xE0) {
            _getch();   // swallow the second byte of arrow/function keys
        }
        else if (ch == 3) {
            std::exit(1);   // Ctrl+C
        }
        else {
            pw.push_back(static_cast<char>(ch));
            std::cout << '*';
        }
    }
    std::cout << '\n';
    return pw;
}

bool confirm(const std::string& question)
{
    std::string answer = prompt(question + " (y/n): ");
    return !answer.empty() && (answer[0] == 'y' || answer[0] == 'Y');
}

// ---------------------------------------------------------------------------
// Database operations -- each takes the open session so it can be reused
// later (e.g. from an HTTP handler) without depending on console input.
// ---------------------------------------------------------------------------

// Columns selected by every user query, in display order.
// DATE columns come back from X DevAPI as raw bytes, so cast to text server-side.
const char* USER_COLUMNS = "id, name, email, CAST(registration_date AS CHAR)";

void printUserRow(mysqlx::Row& row)
{
    std::cout << "  [" << row[0].get<int>() << "] "
              << row[1].get<std::string>()
              << "  <" << (row[2].isNull() ? "no email" : row[2].get<std::string>()) << ">"
              << "  registered " << (row[3].isNull() ? "(unknown)" : row[3].get<std::string>())
              << '\n';
}

// Prints every row in the result; returns how many there were.
int printUsers(mysqlx::SqlResult& result)
{
    int count = 0;
    mysqlx::Row row;
    while ((row = result.fetchOne())) {
        printUserRow(row);
        ++count;
    }
    return count;
}

void findUserByEmail(mysqlx::Session& db)
{
    std::string email = prompt("Email to look up: ");
    if (email.empty()) return;

    // Raw SQL via Session::sql() only supports positional '?' placeholders.
    mysqlx::SqlResult result = db.sql(
        std::string("SELECT ") + USER_COLUMNS + " FROM users WHERE email = ?")
        .bind(email)
        .execute();

    if (printUsers(result) == 0)
        std::cout << "No user found with email " << email << '\n';
}

void searchUsersByName(mysqlx::Session& db)
{
    std::string term = prompt("Name contains: ");
    if (term.empty()) return;

    mysqlx::SqlResult result = db.sql(
        std::string("SELECT ") + USER_COLUMNS + " FROM users WHERE name LIKE ? ORDER BY name")
        .bind("%" + term + "%")
        .execute();

    int n = printUsers(result);
    std::cout << n << " match(es).\n";
}

void listAllUsers(mysqlx::Session& db)
{
    mysqlx::SqlResult result = db.sql(
        std::string("SELECT ") + USER_COLUMNS + " FROM users ORDER BY id")
        .execute();

    int n = printUsers(result);
    std::cout << n << " user(s) total.\n";
}

void addUser(mysqlx::Session& db)
{
    std::string name = prompt("Name: ");
    if (name.empty()) { std::cout << "Cancelled.\n"; return; }

    std::string email = prompt("Email: ");
    if (email.empty()) { std::cout << "Cancelled.\n"; return; }

    mysqlx::SqlResult result = db.sql(
        "INSERT INTO users (name, email, registration_date) VALUES (?, ?, CURDATE())")
        .bind(name, email)
        .execute();

    std::cout << "Added user with id " << result.getAutoIncrementValue() << ".\n";
}

void updateUserName(mysqlx::Session& db)
{
    std::string email = prompt("Email of user to rename: ");
    if (email.empty()) return;

    std::string newName = prompt("New name: ");
    if (newName.empty()) { std::cout << "Cancelled.\n"; return; }

    mysqlx::SqlResult result = db.sql("UPDATE users SET name = ? WHERE email = ?")
        .bind(newName, email)
        .execute();

    if (result.getAffectedItemsCount() == 0)
        std::cout << "No user found with email " << email << '\n';
    else
        std::cout << "Updated.\n";
}

void deleteUser(mysqlx::Session& db)
{
    std::string email = prompt("Email of user to delete: ");
    if (email.empty()) return;

    // Show who will be deleted before asking for confirmation.
    mysqlx::SqlResult found = db.sql(
        std::string("SELECT ") + USER_COLUMNS + " FROM users WHERE email = ?")
        .bind(email)
        .execute();
    if (printUsers(found) == 0) {
        std::cout << "No user found with email " << email << '\n';
        return;
    }

    if (!confirm("Delete this user?")) {
        std::cout << "Cancelled.\n";
        return;
    }

    db.sql("DELETE FROM users WHERE email = ?").bind(email).execute();
    std::cout << "Deleted.\n";
}

// ---------------------------------------------------------------------------
// Menu loop
// ---------------------------------------------------------------------------

void printMenu()
{
    std::cout << "\n=== Main Menu ===\n"
              << "  1) Find user by email\n"
              << "  2) Search users by name\n"
              << "  3) List all users\n"
              << "  4) Add user\n"
              << "  5) Rename user\n"
              << "  6) Delete user\n"
              << "  0) Quit\n";
}

int main()
{
    std::cout << "Welcome to the Library Management System!\n";

    std::string password = promptPassword("Enter DB password: ");

    try {
        std::cout << "Connecting to database at localhost:33060/testdb as root...\n";

        // SessionSettings instead of a "mysqlx://user:pw@host" URL string: a URL
        // breaks if the password contains characters like @ / : or %.
        mysqlx::SessionSettings settings(
            mysqlx::SessionOption::USER, "root",
            mysqlx::SessionOption::PWD, password,
            mysqlx::SessionOption::HOST, "localhost",
            mysqlx::SessionOption::PORT, 33060,
            mysqlx::SessionOption::DB, "testdb",
            mysqlx::SessionOption::SSL_MODE, mysqlx::SSLMode::REQUIRED
        );

        // One session for the whole program run, shared by every command.
        mysqlx::Session db(settings);
        std::cout << "Connected to database successfully!\n";

        bool running = true;
        while (running) {
            printMenu();
            std::string choice = prompt("> ");

            if (!std::cin) break;   // EOF on input -- exit cleanly

            // Catch errors per command so one failed query (bad input, duplicate
            // email, etc.) reports the problem and returns to the menu instead
            // of ending the program.
            try {
                if      (choice == "1") findUserByEmail(db);
                else if (choice == "2") searchUsersByName(db);
                else if (choice == "3") listAllUsers(db);
                else if (choice == "4") addUser(db);
                else if (choice == "5") updateUserName(db);
                else if (choice == "6") deleteUser(db);
                else if (choice == "0" || choice == "q") running = false;
                else if (!choice.empty()) std::cout << "Unknown option: " << choice << '\n';
            }
            catch (const mysqlx::Error& err) {
                std::cerr << "MySQL Error: " << err.what() << '\n';
            }
        }

        db.close();
        std::cout << "Goodbye!\n";
    }
    catch (const mysqlx::Error& err) {
        // Reaching here means the connection itself failed (wrong password, server down).
        std::cerr << "MySQL Error: " << err.what() << '\n';
        return 1;
    }
    catch (const std::exception& exc) {
        std::cerr << "General exception: " << exc.what() << '\n';
        return 1;
    }

    return 0;
}
