#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
using J = nlohmann::json;
int main() {
    try {
        std::string line;
        std::getline(std::cin, line);
        auto req = J::parse(line);
        std::ifstream f("inventory.json");
        auto inventory = J::parse(f);
        auto sku = req["params"].at("sku").get<std::string>();
        for (auto &p : inventory)
            if (p["sku"] == sku) {
                std::cout << J({{"jsonrpc", "2.0"},
                                {"id", req["id"]},
                                {"result",
                                 {{"message", p.at("name").get<std::string>() + ": " +
                                                  std::to_string(p["stock"].get<int>()) +
                                                  " adet stok mevcut."},
                                  {"sku", sku},
                                  {"stock", p["stock"]}}}})
                                 .dump()
                          << "\n";
                return 0;
            }
        std::cout << J({{"jsonrpc", "2.0"},
                        {"id", req["id"]},
                        {"result",
                         {{"message", "Bu ürünün stok bilgisi bulunamadı."}, {"found", false}}}})
                         .dump()
                  << "\n";
        return 0;
    } catch (...) {
        return 1;
    }
}
