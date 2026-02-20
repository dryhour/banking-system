# Banking System
This is a banking system that allows users to create accounts, deposit and withdraw cash, and view balance. It is created using c++ and SQL.
## Testing
Run: cd backend
g++ server.cpp -o server -std=c++11 -I/opt/homebrew/opt/mysql-connector-c++/include -L/opt/homebrew/opt/mysql-connector-c++/lib -lmysqlcppconnx
./server
