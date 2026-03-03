// tiny_exchange_server_asio.cpp (C++23 + Boost.Asio)
// Async TCP server with per-connection sessions and a message->command pipeline.
// Deterministic engine behaviour by serialising all book mutation on a single strand.
//
// Build (Linux/WSL):
// g++ -std=c++23 -O2 -Wall -Wextra -pedantic tiny_exchange_server_asio.cpp -o tiny_exchange -lboost_system -pthread
//
// Run:
// ./tiny_exchange 9001

#include <print>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <unordered_map>
#include <list>
#include <algorithm>
#include <sstream>
#include <deque>
#include <memory>
#include <thread>

#include <boost/asio.hpp>

using tcp = boost::asio::ip::tcp;

enum class CommantType
{
    New, Cancle, PrintBook, Ouit, Unknown
};

enum class Side
{
    BUY = 0, SELL = 1
};

struct Command
{
    CommantType type {CommantType::Unknown};
    Side side {Side::BUY};
    int price {0};
    int Quantity {0};
    int OrderID {0};
};

enum class OrderStatus
{
    New,
    PatiallyFilled,
    Filled,
    Cannclled,
};

class Order
{
private:
    int ID {};
    Side side {};
    int Price {};
    int O_Quantity {};
    int N_Quantity {};
    int TS {};
    OrderStatus status {OrderStatus::New};

public:
    Order() = default;

    Order(int Id, Side s, int price, int o_quantity, int n_quantity, int ts, OrderStatus OS)
        : ID{Id}, side{s}, Price{price}, O_Quantity{o_quantity}, N_Quantity{n_quantity}, TS{ts}, status{OS} {}

public:
    int remaining() const { return N_Quantity; }
    int id() const { return ID; }
    int price() const { return Price; }
    Side get_side() const { return side; }
    int ts() const { return TS; }
    OrderStatus get_status() const { return status; }

    void reduce_remaining(int filled)
    {
        if (filled > 0 && filled <= N_Quantity)
        {
            N_Quantity -= filled;
            if (N_Quantity == 0) status = OrderStatus::Filled;
            else status = OrderStatus::PatiallyFilled;
        }
    }

    void cancel()
    {
        status = OrderStatus::Cannclled;
    }
};

struct Trade
{
    int OrderB_ID {};
    int OrderS_ID {};
    int price {};
    int Quantity {};
    int TS {};
};

struct PriceLevel
{
    std::list<Order> fifo;
};

struct MachingEngine
{
    struct Locator
    {
        Side side {};
        int price {};
        std::list<Order>::iterator it;
    };

    int nextID {1};
    int seqTS {0};

    std::map<int, PriceLevel> Bids; // best bid is rbegin()
    std::map<int, PriceLevel> Ask;  // best ask is begin()

    std::unordered_map<int, Locator> index;
    std::vector<Trade> last_trades;

    int next_ts() { return ++seqTS; }

    const std::vector<Trade>& get_last_trades() const { return last_trades; }

    int NewOrder(const Command& EngineCommand)
    {
        last_trades.clear();

        const int order_id = nextID++;
        int incoming = EngineCommand.Quantity;

        if (EngineCommand.side == Side::BUY)
        {
            while (incoming > 0 && !Ask.empty())
            {
                auto level_it = Ask.begin();
                const int level_price = level_it->first;

                if (level_price > EngineCommand.price) break;

                auto& fifo = level_it->second.fifo;

                while (incoming > 0 && !fifo.empty())
                {
                    auto& maker = fifo.front();

                    const int maker_id = maker.id();
                    const int maker_rem = maker.remaining();
                    const int fill = std::min(incoming, maker_rem);

                    incoming -= fill;
                    maker.reduce_remaining(fill);

                    last_trades.push_back(Trade{order_id, maker_id, level_price, fill, next_ts()});

                    if (maker.remaining() == 0)
                    {
                        index.erase(maker_id);
                        fifo.pop_front();
                    }
                }

                if (fifo.empty())
                {
                    Ask.erase(level_it);
                }
            }

            if (incoming > 0)
            {
                Order rest(order_id, Side::BUY, EngineCommand.price,
                           EngineCommand.Quantity, incoming, next_ts(), OrderStatus::New);

                auto& level = Bids[EngineCommand.price];
                level.fifo.push_back(rest);
                auto it = std::prev(level.fifo.end());
                index[order_id] = Locator{Side::BUY, EngineCommand.price, it};
            }

            return order_id;
        }

        if (EngineCommand.side == Side::SELL)
        {
            while (incoming > 0 && !Bids.empty())
            {
                auto level_it = std::prev(Bids.end());
                const int level_price = level_it->first;

                if (level_price < EngineCommand.price) break;

                auto& fifo = level_it->second.fifo;

                while (incoming > 0 && !fifo.empty())
                {
                    auto& maker = fifo.front();

                    const int maker_id = maker.id();
                    const int maker_rem = maker.remaining();
                    const int fill = std::min(incoming, maker_rem);

                    incoming -= fill;
                    maker.reduce_remaining(fill);

                    last_trades.push_back(Trade{maker_id, order_id, level_price, fill, next_ts()});

                    if (maker.remaining() == 0)
                    {
                        index.erase(maker_id);
                        fifo.pop_front();
                    }
                }

                if (fifo.empty())
                {
                    Bids.erase(level_it);
                }
            }

            if (incoming > 0)
            {
                Order rest(order_id, Side::SELL, EngineCommand.price,
                           EngineCommand.Quantity, incoming, next_ts(), OrderStatus::New);

                auto& level = Ask[EngineCommand.price];
                level.fifo.push_back(rest);
                auto it = std::prev(level.fifo.end());
                index[order_id] = Locator{Side::SELL, EngineCommand.price, it};
            }

            return order_id;
        }

        return order_id;
    }

    bool Cancel(int order_id)
    {
        auto it = index.find(order_id);
        if (it == index.end()) return false;

        const Locator loc = it->second;

        if (loc.side == Side::BUY)
        {
            auto level_it = Bids.find(loc.price);
            if (level_it != Bids.end())
            {
                level_it->second.fifo.erase(loc.it);
                if (level_it->second.fifo.empty()) Bids.erase(level_it);
            }
        }
        else
        {
            auto level_it = Ask.find(loc.price);
            if (level_it != Ask.end())
            {
                level_it->second.fifo.erase(loc.it);
                if (level_it->second.fifo.empty()) Ask.erase(level_it);
            }
        }

        index.erase(it);
        return true;
    }

    std::string PrintBookText(int depth = 10) const
    {
        std::string out;
        out += "BOOK\n";
        out += "ASKS\n";

        int shown = 0;
        for (auto it = Ask.begin(); it != Ask.end() && shown < depth; ++it, ++shown)
        {
            int total = 0;
            for (const auto& o : it->second.fifo) total += o.remaining();
            out += "A " + std::to_string(it->first) + " " + std::to_string(total) + "\n";
        }

        out += "BIDS\n";
        shown = 0;
        for (auto it = Bids.rbegin(); it != Bids.rend() && shown < depth; ++it, ++shown)
        {
            int total = 0;
            for (const auto& o : it->second.fifo) total += o.remaining();
            out += "B " + std::to_string(it->first) + " " + std::to_string(total) + "\n";
        }

        out += "END\n";
        return out;
    }
};

static std::vector<std::string> split_ws(const std::string& s)
{
    std::vector<std::string> out;
    std::istringstream iss(s);
    for (std::string tok; iss >> tok; ) out.push_back(tok);
    return out;
}

static Command parse_command(const std::string& line)
{
    Command cmd;
    auto t = split_ws(line);
    if (t.empty()) return cmd;

    if (t[0] == "NEW" && t.size() == 4)
    {
        cmd.type = CommantType::New;
        cmd.side = (t[1] == "BUY") ? Side::BUY : Side::SELL;
        cmd.price = std::stoi(t[2]);
        cmd.Quantity = std::stoi(t[3]);
        return cmd;
    }

    if (t[0] == "CANCEL" && t.size() == 2)
    {
        cmd.type = CommantType::Cancle;
        cmd.OrderID = std::stoi(t[1]);
        return cmd;
    }

    if ((t[0] == "PRINTBOOK") || (t[0] == "PRINT") || (t.size() == 2 && t[0] == "PRINT" && t[1] == "BOOK"))
    {
        cmd.type = CommantType::PrintBook;
        return cmd;
    }

    if (t[0] == "QUIT")
    {
        cmd.type = CommantType::Ouit;
        return cmd;
    }

    cmd.type = CommantType::Unknown;
    return cmd;
}

struct EngineLoop
{
    boost::asio::strand<boost::asio::io_context::executor_type> strand;
    MachingEngine engine;

    EngineLoop(boost::asio::io_context& io)
        : strand(boost::asio::make_strand(io)) {}

    template<typename Handler>
    void submit(Command cmd, Handler handler)
    {
        boost::asio::post(strand, [this, cmd, handler]() mutable
        {
            std::string resp;

            if (cmd.type == CommantType::New)
            {
                const int id = engine.NewOrder(cmd);

                resp += "ACK " + std::to_string(id) + "\n";

                const auto& trades = engine.get_last_trades();
                for (const auto& tr : trades)
                {
                    resp += "FILL "
                         + std::to_string(tr.OrderB_ID) + " "
                         + std::to_string(tr.OrderS_ID) + " "
                         + std::to_string(tr.price) + " "
                         + std::to_string(tr.Quantity) + " "
                         + std::to_string(tr.TS) + "\n";
                }

                resp += "END\n";
            }
            else if (cmd.type == CommantType::Cancle)
            {
                const bool ok = engine.Cancel(cmd.OrderID);
                if (ok) resp = "CANCEL_OK " + std::to_string(cmd.OrderID) + "\nEND\n";
                else resp = "CANCEL_REJECT " + std::to_string(cmd.OrderID) + "\nEND\n";
            }
            else if (cmd.type == CommantType::PrintBook)
            {
                resp = engine.PrintBookText();
            }
            else if (cmd.type == CommantType::Ouit)
            {
                resp = "BYE\n";
            }
            else
            {
                resp = "ERR Unknown command\nEND\n";
            }

            handler(std::move(resp), cmd.type == CommantType::Ouit);
        });
    }
};

struct Session : std::enable_shared_from_this<Session>
{
    tcp::socket socket;
    EngineLoop& loop;

    boost::asio::streambuf read_buf;
    std::deque<std::string> write_q;

    Session(tcp::socket s, EngineLoop& l)
        : socket(std::move(s)), loop(l) {}

    void start()
    {
        queue_write("WELCOME\nEND\n");
        do_read_line();
    }

    void do_read_line()
    {
        auto self = shared_from_this();

        boost::asio::async_read_until(socket, read_buf, '\n',
            [self](boost::system::error_code ec, std::size_t)
            {
                if (ec) return;

                std::istream is(&self->read_buf);
                std::string line;
                std::getline(is, line);

                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty())
                {
                    self->do_read_line();
                    return;
                }

                Command cmd = parse_command(line);

                self->loop.submit(cmd, [self](std::string resp, bool close_after) mutable
                {
                    self->queue_write(std::move(resp), close_after);
                });
            });
    }

    void queue_write(std::string msg, bool close_after = false)
    {
        bool writing = !write_q.empty();

        if (close_after)
        {
            if (msg.empty() || msg.back() != '\n') msg.push_back('\n');
            msg += "CLOSE\n";
        }

        write_q.push_back(std::move(msg));

        if (!writing) do_write();
    }

    void do_write()
    {
        auto self = shared_from_this();

        boost::asio::async_write(socket, boost::asio::buffer(write_q.front()),
            [self](boost::system::error_code ec, std::size_t)
            {
                if (ec) return;

                bool should_close = false;
                if (self->write_q.front().find("\nCLOSE\n") != std::string::npos) should_close = true;

                self->write_q.pop_front();

                if (should_close)
                {
                    boost::system::error_code ignored;
                    self->socket.shutdown(tcp::socket::shutdown_both, ignored);
                    self->socket.close(ignored);
                    return;
                }

                if (!self->write_q.empty())
                {
                    self->do_write();
                    return;
                }

                self->do_read_line();
            });
    }
};

struct Server
{
    tcp::acceptor acceptor;
    EngineLoop& loop;

    Server(boost::asio::io_context& io, EngineLoop& l, int port)
        : acceptor(io, tcp::endpoint(tcp::v4(), static_cast<unsigned short>(port)))
        , loop(l)
    {}

    void start()
    {
        do_accept();
    }

    void do_accept()
    {
        acceptor.async_accept([this](boost::system::error_code ec, tcp::socket socket)
        {
            if (!ec)
            {
                std::make_shared<Session>(std::move(socket), loop)->start();
            }

            do_accept();
        });
    }
};

int main(int argc, char** argv)
{
    int port = 9001;
    int threads = 1;

    if (argc >= 2) port = std::stoi(argv[1]);
    if (argc >= 3) threads = std::max(1, std::stoi(argv[2]));

    boost::asio::io_context io;
    EngineLoop loop(io);
    Server server(io, loop, port);

    std::println("Tiny Exchange (Boost.Asio) listening on port {} (threads {})", port, threads);

    server.start();

    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threads));
    for (int i = 0; i < threads; ++i)
    {
        pool.emplace_back([&io]() { io.run(); });
    }

    for (auto& t : pool) t.join();
    return 0;
}
