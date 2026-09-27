#include <jami/configurationmanager_interface.h>
#include <jami/conversation_interface.h>
#include <jami/datatransfer_interface.h>
#include <jami/jami.h>
#include <gnutls/gnutls.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

struct Received {
    std::string account;
    std::string conversation;
    std::map<std::string, std::string> message;
};

struct State {
    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> requests;
    std::vector<std::pair<std::string, std::string>> groupRequests;
    std::vector<std::pair<std::string, std::string>> ready;
    std::vector<Received> messages;
};

template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::seconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(200ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

std::string username(const std::string& account)
{
    const auto details = DRing::getAccountDetails(account);
    const auto it = details.find("Account.username");
    return it == details.end() ? std::string {} : it->second;
}

std::string registration(const std::string& account)
{
    const auto details = DRing::getVolatileAccountDetails(account);
    const auto it = details.find("Account.registrationStatus");
    return it == details.end() ? std::string {} : it->second;
}

std::string directConversation(const std::string& account, const std::string& peer)
{
    for (const auto& conversation : DRing::getConversations(account)) {
        const auto members = DRing::getConversationMembers(account, conversation);
        if (members.size() != 2) continue;
        for (const auto& member : members) {
            const auto it = member.find("uri");
            if (it != member.end() && it->second == peer) return conversation;
        }
    }
    return {};
}

std::map<std::string, std::string> accountDetails(const std::string& bootstrap)
{
    return {{"Account.type", "RING"},
            {"Account.hostname", bootstrap},
            {"Account.bootstrapListUrl", ""},
            {"Account.dhtProxyListUrl", ""},
            {"Account.proxyEnabled", "false"},
            {"Account.peerDiscovery", "true"},
            {"Account.accountDiscovery", "true"},
            {"Account.accountPublish", "true"},
            {"Account.upnpEnabled", "false"},
            {"STUN.enable", "false"},
            {"TURN.enable", "false"},
            {"RingNS.uri", ""}};
}
}

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "usage: P2PMessengerDaemonE2E <test-data-root> <private-bootstrap-host:port|--direct>\n";
        return 2;
    }
    const auto root = std::filesystem::path(argv[1]);
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto session = root / suffix;
    const auto data = session / "data";
    const auto config = session / "config";
    std::filesystem::create_directories(data);
    std::filesystem::create_directories(config);
    _putenv_s("JAMI_DATA_HOME", data.string().c_str());
    _putenv_s("JAMI_CONFIG_HOME", config.string().c_str());
    State state;
    bool started = false;
    const auto finish = [&] {
        DRing::unregisterSignalHandlers();
        if (started) DRing::fini();
        gnutls_global_deinit();
    };
    if (gnutls_global_init() != GNUTLS_E_SUCCESS ||
        !DRing::init(static_cast<DRing::InitFlag>(0))) {
        std::cerr << "engine initialization failed\n";
        return 1;
    }
    started = true;
    DRing::registerSignalHandlers({
        DRing::exportable_callback<DRing::ConfigurationSignal::IncomingTrustRequest>(
            [&state](const std::string& account, const std::string& from,
                     const std::string&, const std::vector<uint8_t>&, time_t) {
                std::lock_guard lock(state.mutex);
                state.requests.emplace_back(account, from);
            }),
        DRing::exportable_callback<DRing::ConversationSignal::MessageReceived>(
            [&state](const std::string& account, const std::string& conversation,
                     std::map<std::string, std::string> message) {
                std::lock_guard lock(state.mutex);
                state.messages.push_back({account, conversation, std::move(message)});
            }),
        DRing::exportable_callback<DRing::ConversationSignal::ConversationReady>(
            [&state](const std::string& account, const std::string& conversation) {
                std::lock_guard lock(state.mutex);
                state.ready.emplace_back(account, conversation);
            }),
        DRing::exportable_callback<DRing::ConversationSignal::ConversationRequestReceived>(
            [&state](const std::string& account, const std::string& conversation,
                     std::map<std::string, std::string>) {
                std::lock_guard lock(state.mutex);
                state.groupRequests.emplace_back(account, conversation);
            })});
    if (!DRing::start()) {
        std::cerr << "engine start failed\n";
        finish();
        return 1;
    }
    const bool direct = std::string_view(argv[2]) == "--direct";
    auto aliceDetails = accountDetails(direct ? "" : argv[2]);
    if (direct) aliceDetails["DHT.port"] = "4222";
    const auto alice = DRing::addAccount(aliceDetails);
    std::string bobBootstrap = argv[2];
    if (direct) {
        if (!waitFor([&] { return !username(alice).empty(); }, 45s)) {
            std::cerr << "first peer did not create its identity\n";
            finish();
            return 1;
        }
        // A lone node may remain TRYING until another node joins it.
        std::this_thread::sleep_for(2s);
        bobBootstrap = "127.0.0.1:4222";
        std::cout << "bootstrapping second peer directly from first: " << bobBootstrap << '\n';
    }
    auto bobDetails = accountDetails(bobBootstrap);
    if (direct) bobDetails["DHT.port"] = "4224";
    const auto bob = DRing::addAccount(bobDetails);
    if (alice.empty() || bob.empty() ||
        !waitFor([&] { return !username(alice).empty() && !username(bob).empty(); }, 45s)) {
        std::cerr << "identity creation timed out\n";
        finish();
        return 1;
    }
    const auto aliceUri = username(alice);
    const auto bobUri = username(bob);
    std::cout << "identities ready\n";
    if (!waitFor([&] { return registration(alice) == "REGISTERED" &&
                              registration(bob) == "REGISTERED"; }, 30s)) {
        std::cerr << "DHT registration not ready: alice=" << registration(alice)
                  << " bob=" << registration(bob) << '\n';
        finish();
        return 1;
    }
    std::cout << "DHT registered\n";
    if (direct) {
        const auto first = DRing::getAccountDetails(alice);
        const auto second = DRing::getAccountDetails(bob);
        if (first.at("Account.hostname") != "" ||
            second.at("Account.hostname") != bobBootstrap ||
            first.at("TURN.enable") != "false" ||
            second.at("TURN.enable") != "false" ||
            first.at("Account.proxyEnabled") != "false" ||
            second.at("Account.proxyEnabled") != "false") {
            std::cerr << "direct test unexpectedly configured an intermediary\n";
            finish();
            return 1;
        }
    }
    DRing::sendTrustRequest(alice, bobUri);
    std::string requestFrom;
    bool liveRequestSignal = false;
    if (!waitFor([&] {
            {
                std::lock_guard lock(state.mutex);
                for (const auto& request : state.requests)
                    if (request.first == bob && request.second == aliceUri) {
                        requestFrom = request.second;
                        liveRequestSignal = true;
                    }
            }
            for (const auto& request : DRing::getTrustRequests(bob)) {
                const auto from = request.find("from");
                if (from != request.end()) requestFrom = from->second;
            }
            return !requestFrom.empty();
        }, 60s)) {
        std::cerr << "friend request not delivered: alice=" << registration(alice)
                  << " bob=" << registration(bob)
                  << " bob_pending=" << DRing::getTrustRequests(bob).size() << '\n';
        finish();
        return 1;
    }
    if (!liveRequestSignal) {
        std::cerr << "request persisted but live notification was not correct\n";
        finish();
        return 1;
    }
    std::cout << "friend request received from " << requestFrom << '\n';
    if (!DRing::acceptTrustRequest(bob, requestFrom) ||
        !waitFor([&] {
            return !directConversation(alice, bobUri).empty() &&
                   !directConversation(bob, aliceUri).empty();
        }, 40s)) {
        std::cerr << "friend request acceptance failed\n";
        finish();
        return 1;
    }
    std::cout << "friend request accepted\n";
    const auto conversation = directConversation(alice, bobUri);
    if (!waitFor([&] {
            std::lock_guard lock(state.mutex);
            for (const auto& ready : state.ready)
                if (ready.first == bob && ready.second == conversation) return true;
            return false;
        }, 35s)) {
        std::cerr << "recipient conversation clone not ready\n";
        finish();
        return 1;
    }
    std::cout << "recipient conversation ready\n";
    const std::string text = "P2P_MESSENGER_E2E_TEXT_" + suffix;
    DRing::sendMessage(alice, conversation, text, {});
    if (!waitFor([&] {
            std::lock_guard lock(state.mutex);
            for (const auto& received : state.messages) {
                const auto body = received.message.find("body");
                if (received.account == bob && body != received.message.end() && body->second == text)
                    return true;
            }
            return false;
        }, 60s)) {
        {
            std::lock_guard lock(state.mutex);
            std::cerr << "text message not delivered; observed " << state.messages.size() << " events\n";
            for (const auto& received : state.messages) {
                const auto body = received.message.find("body");
                const auto type = received.message.find("type");
                std::cerr << "  account=" << received.account << " type="
                          << (type == received.message.end() ? "" : type->second)
                          << " body=" << (body == received.message.end() ? "" : body->second) << '\n';
            }
        }
        finish();
        return 1;
    }
    std::cout << "text delivered\n";
    const auto source = session / "test-file.txt";
    const auto destination = session / "received-file.txt";
    { std::ofstream output(source, std::ios::binary); output << text; }
    DRing::sendFile(alice, conversation, source.string(), {}, {});
    std::string interaction;
    std::string fileId;
    if (!waitFor([&] {
            std::lock_guard lock(state.mutex);
            for (const auto& received : state.messages) {
                if (received.account != bob) continue;
                const auto type = received.message.find("type");
                const auto id = received.message.find("id");
                const auto file = received.message.find("fileId");
                if (type != received.message.end() && type->second == "application/data-transfer+json" &&
                    id != received.message.end() && file != received.message.end()) {
                    interaction = id->second;
                    fileId = file->second;
                    return true;
                }
            }
            return false;
        }, 60s) ||
        !DRing::downloadFile(bob, directConversation(bob, aliceUri), interaction,
                             fileId, destination.string()) ||
        !waitFor([&] {
            if (!std::filesystem::exists(destination)) return false;
            std::ifstream input(destination, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), {}) == text;
        }, 60s)) {
        std::cerr << "file transfer not completed\n";
        finish();
        return 1;
    }
    std::cout << "file delivered\n";
    const auto group = DRing::startConversation(alice);
    if (group.empty()) {
        std::cerr << "group creation failed\n";
        finish();
        return 1;
    }
    DRing::updateConversationInfos(alice, group, {{"title", "E2E group"}});
    DRing::addConversationMember(alice, group, bobUri);
    if (!waitFor([&] {
            std::lock_guard lock(state.mutex);
            for (const auto& request : state.groupRequests)
                if (request.first == bob && request.second == group) return true;
            return false;
        }, 45s)) {
        std::cerr << "group invitation not delivered\n";
        finish();
        return 1;
    }
    DRing::acceptConversationRequest(bob, group);
    if (!waitFor([&] {
            std::lock_guard lock(state.mutex);
            for (const auto& ready : state.ready)
                if (ready.first == bob && ready.second == group) return true;
            return false;
        }, 35s)) {
        std::cerr << "group conversation did not become ready\n";
        finish();
        return 1;
    }
    const auto groupText = text + "_GROUP";
    DRing::sendMessage(alice, group, groupText, {});
    if (!waitFor([&] {
            std::lock_guard lock(state.mutex);
            for (const auto& received : state.messages) {
                const auto body = received.message.find("body");
                if (received.account == bob && received.conversation == group &&
                    body != received.message.end() && body->second == groupText) return true;
            }
            return false;
        }, 60s)) {
        std::cerr << "group text not delivered\n";
        finish();
        return 1;
    }
    std::cout << "group invitation and text delivered\n";
    finish();
    return 0;
}
