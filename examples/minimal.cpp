#include <cstdio>
#include <string>

#include "erikslund/http/http.hpp"

int main() {
    using namespace erikslund::http;

    Router router;
    router.get("/", [](const Request&) { return Response::text("hello\n"); });

    Server server(std::move(router), ServerOptions::on_port(8'080));
    server.start();

    // Closing stdin stops this example; services should wait for a signal.
    static_cast<void>(std::getchar());
}
