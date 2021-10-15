
#ifndef REALM_NOINST_CLIENT_IMPL_BASE_HPP
#define REALM_NOINST_CLIENT_IMPL_BASE_HPP

#include <cstdint>
#include <utility>
#include <functional>
#include <deque>
#include <map>
#include <string>
#include <random>

#include <realm/binary_data.hpp>
#include <realm/util/optional.hpp>
#include <realm/util/buffer_stream.hpp>
#include <realm/util/logger.hpp>
#include <realm/util/network_ssl.hpp>
#include <realm/util/websocket.hpp>
#include <realm/sync/noinst/client_history_impl.hpp>
#include <realm/sync/noinst/protocol_codec.hpp>
#include <realm/sync/noinst/client_reset_operation.hpp>
#include <realm/sync/protocol.hpp>
#include <realm/sync/history.hpp>


namespace realm {
namespace sync {

// (protocol, address, port, session_multiplex_ident)
//
// `protocol` is included for convenience, even though it is not strictly part
// of an endpoint.
using ServerEndpoint = std::tuple<ProtocolEnvelope, std::string, util::network::Endpoint::port_type, std::string>;

class SessionWrapper;

class SessionWrapperStack {
public:
    bool empty() const noexcept;
    void push(util::bind_ptr<SessionWrapper>) noexcept;
    util::bind_ptr<SessionWrapper> pop() noexcept;
    void clear() noexcept;
    SessionWrapperStack() noexcept = default;
    SessionWrapperStack(SessionWrapperStack&&) noexcept;
    ~SessionWrapperStack();
    friend void swap(SessionWrapperStack& q_1, SessionWrapperStack& q_2) noexcept
    {
        std::swap(q_1.m_back, q_2.m_back);
    }

private:
    SessionWrapper* m_back = nullptr;
};

/// The presence of the ClientReset config indicates an ongoing or requested client
/// reset operation. If client_reset is util::none or if the local Realm does not
/// exist, an ordinary sync session will take place.
///
/// A session will perform client reset by downloading a fresh copy of the Realm
/// from the server at a different file path location. After download, the fresh
/// Realm will be integrated into the local Realm in a write transaction. The
/// application is free to read or write to the local realm during the entire client
/// reset. Like a DOWNLOAD message, the application will not be able to perform a
/// write transaction at the same time as the sync client performs its own write
/// transaction. Client reset is not more disturbing for the application than any
/// DOWNLOAD message. The application can listen to change notifications from the
/// client reset exactly as in a DOWNLOAD message. If the application writes to the
/// local realm during client reset but before the client reset operation has
/// obtained a write lock, the changes made by the application may be lost or
/// overwritten depending on the recovery mode selected.
///
/// Client reset downloads its fresh Realm copy for a Realm at path "xyx.realm" to
/// "xyz.realm.fresh". It is assumed that this path is available for use and if
/// there are any problems the client reset will fail with
/// Client::Error::client_reset_failed.
///
/// The recommended usage of client reset is after a previous session encountered an
/// error that implies the need for a client reset. It is not recommended to persist
/// the need for a client reset. The application should just attempt to synchronize
/// in the usual fashion and only after hitting an error, start a new session with a
/// client reset. In other words, if the application crashes during a client reset,
/// the application should attempt to perform ordinary synchronization after restart
/// and switch to client reset if needed.
///
/// Error codes that imply the need for a client reset are the session level error
/// codes described by SyncError::is_client_reset_requested()
///
/// However, other errors such as bad changeset (UPLOAD) could also be resolved with
/// a client reset. Client reset can even be used without any prior error if so
/// desired.
///
/// After completion of a client reset, the sync client will continue synchronizing
/// with the server in the usual fashion.
///
/// The progress of client reset can be tracked with the standard progress handler.
///
/// Client reset is done when the progress handler arguments satisfy
/// "progress_version > 0". However, if the application wants to ensure that it has
/// all data present on the server, it should wait for download completion using
/// either void async_wait_for_download_completion(WaitOperCompletionHandler) or
/// bool wait_for_download_complete_or_client_stopped().
struct ClientReset {
    bool seamless_loss = false;
    DBRef fresh_copy;
    util::UniqueFunction<void(TransactionRef local, TransactionRef remote)> notify_before_client_reset;
    util::UniqueFunction<void(TransactionRef local)> notify_after_client_reset;
};

/// \brief Protocol errors discovered by the client.
///
/// These errors will terminate the network connection (disconnect all sessions
/// associated with the affected connection), and the error will be reported to
/// the application via the connection state change listeners of the affected
/// sessions.
enum class ClientError {
    // clang-format off
    connection_closed           = 100, ///< Connection closed (no error)
    unknown_message             = 101, ///< Unknown type of input message
    bad_syntax                  = 102, ///< Bad syntax in input message head
    limits_exceeded             = 103, ///< Limits exceeded in input message
    bad_session_ident           = 104, ///< Bad session identifier in input message
    bad_message_order           = 105, ///< Bad input message order
    bad_client_file_ident       = 106, ///< Bad client file identifier (IDENT)
    bad_progress                = 107, ///< Bad progress information (DOWNLOAD)
    bad_changeset_header_syntax = 108, ///< Bad syntax in changeset header (DOWNLOAD)
    bad_changeset_size          = 109, ///< Bad changeset size in changeset header (DOWNLOAD)
    bad_origin_file_ident       = 110, ///< Bad origin file identifier in changeset header (DOWNLOAD)
    bad_server_version          = 111, ///< Bad server version in changeset header (DOWNLOAD)
    bad_changeset               = 112, ///< Bad changeset (DOWNLOAD)
    bad_request_ident           = 113, ///< Bad request identifier (MARK)
    bad_error_code              = 114, ///< Bad error code (ERROR),
    bad_compression             = 115, ///< Bad compression (DOWNLOAD)
    bad_client_version          = 116, ///< Bad last integrated client version in changeset header (DOWNLOAD)
    ssl_server_cert_rejected    = 117, ///< SSL server certificate rejected
    pong_timeout                = 118, ///< Timeout on reception of PONG respone message
    bad_client_file_ident_salt  = 119, ///< Bad client file identifier salt (IDENT)
    bad_file_ident              = 120, ///< Bad file identifier (ALLOC)
    connect_timeout             = 121, ///< Sync connection was not fully established in time
    bad_timestamp               = 122, ///< Bad timestamp (PONG)
    bad_protocol_from_server    = 123, ///< Bad or missing protocol version information from server
    client_too_old_for_server   = 124, ///< Protocol version negotiation failed: Client is too old for server
    client_too_new_for_server   = 125, ///< Protocol version negotiation failed: Client is too new for server
    protocol_mismatch           = 126, ///< Protocol version negotiation failed: No version supported by both client and server
    bad_state_message           = 127, ///< Bad values in state message (STATE)
    missing_protocol_feature    = 128, ///< Requested feature missing in negotiated protocol version
    http_tunnel_failed          = 131, ///< Failed to establish HTTP tunnel with configured proxy
    auto_client_reset_failure   = 132, ///< A fatal error was encountered which prevents completion of a client reset
    // clang-format on
};

const std::error_category& client_error_category() noexcept;

std::error_code make_error_code(ClientError) noexcept;
} // namespace sync
} // namespace realm

namespace std {

template <>
struct is_error_code_enum<realm::sync::ClientError> {
    static const bool value = true;
};

} // namespace std

namespace realm {
namespace sync {

static constexpr milliseconds_type default_connect_timeout = 120000;        // 2 minutes
static constexpr milliseconds_type default_connection_linger_time = 30000;  // 30 seconds
static constexpr milliseconds_type default_ping_keepalive_period = 60000;   // 1 minute
static constexpr milliseconds_type default_pong_keepalive_timeout = 120000; // 2 minutes
static constexpr milliseconds_type default_fast_reconnect_limit = 60000;    // 1 minute

using RoundtripTimeHandler = void(milliseconds_type roundtrip_time);

struct ClientConfig {
    /// An optional custom platform description to be sent to server as part
    /// of a user agent description (HTTP `User-Agent` header).
    ///
    /// If left empty, the platform description will be whatever is returned
    /// by util::get_platform_info().
    std::string user_agent_platform_info;

    /// Optional information about the application to be added to the user
    /// agent description as sent to the server. The intention is that the
    /// application describes itself using the following (rough) syntax:
    ///
    ///     <application info>  ::=  (<space> <layer>)*
    ///     <layer>             ::=  <name> "/" <version> [<space> <details>]
    ///     <name>              ::=  (<alnum>)+
    ///     <version>           ::=  <digit> (<alnum> | "." | "-" | "_")*
    ///     <details>           ::=  <parentherized>
    ///     <parentherized>     ::=  "(" (<nonpar> | <parentherized>)* ")"
    ///
    /// Where `<space>` is a single space character, `<digit>` is a decimal
    /// digit, `<alnum>` is any alphanumeric character, and `<nonpar>` is
    /// any character other than `(` and `)`.
    ///
    /// When multiple levels are present, the innermost layer (the one that
    /// is closest to this API) should appear first.
    ///
    /// Example:
    ///
    ///     RealmJS/2.13.0 RealmStudio/2.9.0
    ///
    /// Note: The user agent description is not intended for machine
    /// interpretation, but should still follow the specified syntax such
    /// that it remains easily interpretable by human beings.
    std::string user_agent_application_info;

    /// An optional logger to be used by the client. If no logger is
    /// specified, the client will use an instance of util::StderrLogger
    /// with the log level threshold set to util::Logger::Level::info. The
    /// client does not require a thread-safe logger, and it guarantees that
    /// all logging happens either on behalf of the constructor or on behalf
    /// of the invocation of run().
    util::Logger* logger = nullptr;

    /// Use ports 80 and 443 by default instead of 7800 and 7801
    /// respectively. Ideally, these default ports should have been made
    /// available via a different URI scheme instead (http/https or ws/wss).
    bool enable_default_port_hack = true;

    /// For testing purposes only.
    ReconnectMode reconnect_mode = ReconnectMode::normal;

    /// Create a separate connection for each session. For testing purposes
    /// only.
    ///
    /// FIXME: This setting needs to be true for now, due to limitations in
    /// the load balancer.
    bool one_connection_per_session = true;

    /// Do not access the local file system. Sessions will act as if
    /// initiated on behalf of an empty (or nonexisting) local Realm
    /// file. Received DOWNLOAD messages will be accepted, but otherwise
    /// ignored. No UPLOAD messages will be generated. For testing purposes
    /// only.
    ///
    /// Many operations, such as serialized transactions, are not suppored
    /// in this mode.
    bool dry_run = false;

    /// The maximum number of milliseconds to allow for a connection to
    /// become fully established. This includes the time to resolve the
    /// network address, the TCP connect operation, the SSL handshake, and
    /// the WebSocket handshake.
    milliseconds_type connect_timeout = default_connect_timeout;

    /// The number of milliseconds to keep a connection open after all
    /// sessions have been abandoned (or suspended by errors).
    ///
    /// The purpose of this linger time is to avoid close/reopen cycles
    /// during short periods of time where there are no sessions interested
    /// in using the connection.
    ///
    /// If the connection gets closed due to an error before the linger time
    /// expires, the connection will be kept closed until there are sessions
    /// willing to use it again.
    milliseconds_type connection_linger_time = default_connection_linger_time;

    /// The client will send PING messages periodically to allow the server
    /// to detect dead connections (heartbeat). This parameter specifies the
    /// time, in milliseconds, between these PING messages. When scheduling
    /// the next PING message, the client will deduct a small random amount
    /// from the specified value to help spread the load on the server from
    /// many clients.
    milliseconds_type ping_keepalive_period = default_ping_keepalive_period;

    /// Whenever the server receives a PING message, it is supposed to
    /// respond with a PONG messsage to allow the client to detect dead
    /// connections (heartbeat). This parameter specifies the time, in
    /// milliseconds, that the client will wait for the PONG response
    /// message before it assumes that the connection is dead, and
    /// terminates it.
    milliseconds_type pong_keepalive_timeout = default_pong_keepalive_timeout;

    /// The maximum amount of time, in milliseconds, since the loss of a
    /// prior connection, for a new connection to be considered a *fast
    /// reconnect*.
    ///
    /// In general, when a client establishes a connection to the server,
    /// the uploading process remains suspended until the initial
    /// downloading process completes (as if by invocation of
    /// Session::async_wait_for_download_completion()). However, to avoid
    /// unnecessary latency in change propagation during ongoing
    /// application-level activity, if the new connection is established
    /// less than a certain amount of time (`fast_reconnect_limit`) since
    /// the client was previously connected to the server, then the
    /// uploading process will be activated immediately.
    ///
    /// For now, the purpose of the general delaying of the activation of
    /// the uploading process, is to increase the chance of multiple initial
    /// transactions on the client-side, to be uploaded to, and processed by
    /// the server as a single unit. In the longer run, the intention is
    /// that the client should upload transformed (from reciprocal history),
    /// rather than original changesets when applicable to reduce the need
    /// for changeset to be transformed on both sides. The delaying of the
    /// upload process will increase the number of cases where this is
    /// possible.
    ///
    /// FIXME: Currently, the time between connections is not tracked across
    /// sessions, so if the application closes its session, and opens a new
    /// one immediately afterwards, the activation of the upload process
    /// will be delayed unconditionally.
    milliseconds_type fast_reconnect_limit = default_fast_reconnect_limit;

    /// Set to true to completely disable delaying of the upload process. In
    /// this mode, the upload process will be activated immediately, and the
    /// value of `fast_reconnect_limit` is ignored.
    ///
    /// For testing purposes only.
    bool disable_upload_activation_delay = false;

    /// If `disable_upload_compaction` is true, every changeset will be
    /// compacted before it is uploaded to the server. Compaction will
    /// reduce the size of a changeset if the same field is set multiple
    /// times or if newly created objects are deleted within the same
    /// transaction. Log compaction increeses CPU usage and memory
    /// consumption.
    bool disable_upload_compaction = false;

    /// Set the `TCP_NODELAY` option on all TCP/IP sockets. This disables
    /// the Nagle algorithm. Disabling it, can in some cases be used to
    /// decrease latencies, but possibly at the expense of scalability. Be
    /// sure to research the subject before you enable this option.
    bool tcp_no_delay = false;

    /// The specified function will be called whenever a PONG message is
    /// received on any connection. The round-trip time in milliseconds will
    /// be pased to the function. The specified function will always be
    /// called by the client's event loop thread, i.e., the thread that
    /// calls `Client::run()`. This feature is mainly for testing purposes.
    std::function<RoundtripTimeHandler> roundtrip_time_handler;

    /// Disable sync to disk (fsync(), msync()) for all realm files managed
    /// by this client.
    ///
    /// Testing/debugging feature. Should never be enabled in production.
    bool disable_sync_to_disk = false;
};

/// \brief Information about an error causing a session to be temporarily
/// disconnected from the server.
///
/// In general, the connection will be automatically reestablished
/// later. Whether this happens quickly, generally depends on \ref
/// is_fatal. If \ref is_fatal is true, it means that the error is deemed to
/// be of a kind that is likely to persist, and cause all future reconnect
/// attempts to fail. In that case, if another attempt is made at
/// reconnecting, the delay will be substantial (at least an hour).
///
/// \ref error_code specifies the error that caused the connection to be
/// closed. For the list of errors reported by the server, see \ref
/// ProtocolError (or `protocol.md`). For the list of errors corresponding
/// to protocol violations that are detected by the client, see
/// Client::Error. The error may also be a system level error, or an error
/// from one of the potential intermediate protocol layers (SSL or
/// WebSocket).
///
/// \ref detailed_message is the most detailed message available to describe
/// the error. It is generally equal to `error_code.message()`, but may also
/// be a more specific message (one that provides extra context). The
/// purpose of this message is mostly to aid in debugging. For non-debugging
/// purposes, `error_code.message()` should generally be considered
/// sufficient.
///
/// \sa set_connection_state_change_listener().
struct SessionErrorInfo {
    std::error_code error_code;
    bool is_fatal;
    const std::string& detailed_message;
};

enum class ConnectionState { disconnected, connecting, connected };

class ClientImpl {
public:
    enum class ConnectionTerminationReason;
    class Connection;
    class Session;

    using port_type = util::network::Endpoint::port_type;
    using OutputBuffer         = util::ResettableExpandableBufferOutputStream;
    using ClientProtocol = _impl::ClientProtocol;
    using ClientResetOperation = _impl::ClientResetOperation;
    using EventLoopMetricsHandler = util::network::Service::EventLoopMetricsHandler;

    /// Per-server endpoint information used to determine reconnect delays.
    class ReconnectInfo {
    public:
        void reset() noexcept;

    private:
        using milliseconds_lim = std::numeric_limits<milliseconds_type>;

        // When `m_reason` is present, it indicates that a connection attempt was
        // initiated, and that a new reconnect delay must be computed before
        // initiating another connection attempt. In this case, `m_time_point` is
        // the point in time from which the next delay should count. It will
        // generally be the time at which the last connection attempt was initiated,
        // but for certain connection termination reasons, it will instead be the
        // time at which the connection was closed. `m_delay` will generally be the
        // duration of the delay that preceded the last connection attempt, and can
        // be used as a basis for computing the next delay.
        //
        // When `m_reason` is absent, it indicates that a new reconnect delay has
        // been computed, and `m_time_point` will be the time at which the delay
        // expires (if equal to `milliseconds_lim::max()`, the delay is
        // indefinite). `m_delay` will generally be the duration of the computed
        // delay.
        //
        // Since `m_reason` is absent, and `m_timepoint` is zero initially, the
        // first reconnect delay will already have expired, so the effective delay
        // will be zero.
        util::Optional<ConnectionTerminationReason> m_reason;
        milliseconds_type m_time_point = 0;
        milliseconds_type m_delay = 0;

        // Set this flag to true to schedule a postponed invocation of reset(). See
        // Connection::cancel_reconnect_delay() for details and rationale.
        //
        // Will be set back to false when a PONG message arrives, and the
        // corresponding PING message was sent while `m_scheduled_reset` was
        // true. See receive_pong().
        bool m_scheduled_reset = false;

        friend class Connection;
    };

    static constexpr milliseconds_type default_connect_timeout = 120000;        // 2 minutes
    static constexpr milliseconds_type default_connection_linger_time = 30000;  // 30 seconds
    static constexpr milliseconds_type default_ping_keepalive_period = 60000;   // 1 minute
    static constexpr milliseconds_type default_pong_keepalive_timeout = 120000; // 2 minutes
    static constexpr milliseconds_type default_fast_reconnect_limit = 60000;    // 1 minute

    util::Logger& logger;

    ClientImpl(ClientConfig);
    ~ClientImpl();

    static constexpr int get_oldest_supported_protocol_version() noexcept;

    // @{
    /// These call stop(), run(), and report_event_loop_metrics() on the service
    /// object (get_service()) respectively.
    void stop() noexcept;
    void run();
    void report_event_loop_metrics(std::function<EventLoopMetricsHandler>);
    // @}

    const std::string& get_user_agent_string() const noexcept;
    ReconnectMode get_reconnect_mode() const noexcept;
    bool is_dry_run() const noexcept;
    bool get_tcp_no_delay() const noexcept;
    util::network::Service& get_service() noexcept;
    std::mt19937_64& get_random() noexcept;

    /// Returns false if the specified URL is invalid.
    bool decompose_server_url(const std::string& url, ProtocolEnvelope& protocol, std::string& address,
                              port_type& port, std::string& path) const;

    void cancel_reconnect_delay();
    bool wait_for_session_terminations_or_client_stopped();

private:
    using connection_ident_type = std::int_fast64_t;

    const ReconnectMode m_reconnect_mode; // For testing purposes only
    const milliseconds_type m_connect_timeout;
    const milliseconds_type m_connection_linger_time;
    const milliseconds_type m_ping_keepalive_period;
    const milliseconds_type m_pong_keepalive_timeout;
    const milliseconds_type m_fast_reconnect_limit;
    const bool m_disable_upload_activation_delay;
    const bool m_dry_run; // For testing purposes only
    const bool m_tcp_no_delay;
    const bool m_enable_default_port_hack;
    const bool m_disable_upload_compaction;
    const std::function<RoundtripTimeHandler> m_roundtrip_time_handler;
    const std::string m_user_agent_string;
    util::network::Service m_service;
    std::mt19937_64 m_random;
    ClientProtocol m_client_protocol;
    session_ident_type m_prev_session_ident = 0;

    const bool m_one_connection_per_session;
    util::network::Trigger m_actualize_and_finalize;
    util::network::DeadlineTimer m_keep_running_timer;

    // Note: There is one server slot per server endpoint (hostname, port,
    // session_multiplex_ident), and it survives from one connection object to
    // the next, which is important because it carries information about a
    // possible reconnect delay applying to the new connection object (server
    // hammering protection).
    //
    // Note: Due to a particular load balancing scheme that is currently in use,
    // every session is forced to open a seperate connection (via abuse of
    // `m_one_connection_per_session`, which is only intended for testing
    // purposes). This disables part of the hammering protection scheme built in
    // to the client.
    struct ServerSlot {
        ReconnectInfo reconnect_info; // Applies exclusively to `connection`.
        std::unique_ptr<ClientImpl::Connection> connection;

        // Used instead of `connection` when `m_one_connection_per_session` is
        // true.
        std::map<connection_ident_type, std::unique_ptr<ClientImpl::Connection>> alt_connections;
    };

    // Must be accessed only by event loop thread
    std::map<ServerEndpoint, ServerSlot> m_server_slots;

    // Must be accessed only by event loop thread
    connection_ident_type m_prev_connection_ident = 0;

    util::Mutex m_mutex;

    bool m_stopped = false;                       // Protected by `m_mutex`
    bool m_sessions_terminated = false;           // Protected by `m_mutex`
    bool m_actualize_and_finalize_needed = false; // Protected by `m_mutex`

    std::atomic<bool> m_running{false}; // Debugging facility

    // The set of session wrappers that are not yet wrapping a session object,
    // and are not yet abandoned (still referenced by the application).
    //
    // Protected by `m_mutex`.
    std::map<SessionWrapper*, ServerEndpoint> m_unactualized_session_wrappers;

    // The set of session wrappers that were successfully actualized, but are
    // now abandoned (no longer referenced by the application), and have not yet
    // been finalized. Order in queue is immaterial.
    //
    // Protected by `m_mutex`.
    SessionWrapperStack m_abandoned_session_wrappers;

    // Protected by `m_mutex`.
    util::CondVar m_wait_or_client_stopped_cond;

    void start_keep_running_timer();
    void register_unactualized_session_wrapper(SessionWrapper*, ServerEndpoint);
    void register_abandoned_session_wrapper(util::bind_ptr<SessionWrapper>) noexcept;
    void actualize_and_finalize_session_wrappers();

    // Get or create a connection. If a connection exists for the specified
    // endpoint, it will be returned, otherwise a new connection will be
    // created. If `m_one_connection_per_session` is true (testing only), a new
    // connection will be created every time.
    //
    // Must only be accessed from event loop thread.
    //
    // FIXME: Passing these SSL parameters here is confusing at best, since they
    // are ignored if a connection is already available for the specified
    // endpoint. Also, there is no way to check that all the specified SSL
    // parameters are in agreement with a preexisting connection. A better
    // approach would be to allow for per-endpoint SSL parameters to be
    // specifiable through public member functions of ClientImpl from where they
    // could then be picked up as new connections are created on demand.
    //
    // FIXME: `session_multiplex_ident` should be eliminated from ServerEndpoint
    // as it effectively disables part of the hammering protection scheme if it
    // is used to ensure that each session gets a separate connection. With the
    // alternative approach outlined in the previous FIXME (specify per endpoint
    // SSL parameters at the client object level), there seems to be no more use
    // for `session_multiplex_ident`.
    ClientImpl::Connection& get_connection(ServerEndpoint, const std::string& authorization_header_name,
                                           const std::map<std::string, std::string>& custom_http_headers,
                                           bool verify_servers_ssl_certificate,
                                           util::Optional<std::string> ssl_trust_certificate_path,
                                           std::function<SyncConfig::SSLVerifyCallback>,
                                           util::Optional<SyncConfig::ProxyConfig>, bool& was_created);

    // Destroys the specified connection.
    void remove_connection(ClientImpl::Connection&) noexcept;

    static std::string make_user_agent_string(ClientConfig&);

    session_ident_type get_next_session_ident() noexcept;

    friend class ClientImpl::Connection;
    friend class SessionWrapper;
};

constexpr int ClientImpl::get_oldest_supported_protocol_version() noexcept
{
    // See get_current_protocol_version() for information about the
    // individual protocol versions.
    return 2;
}

static_assert(ClientImpl::get_oldest_supported_protocol_version() >= 1, "");
static_assert(ClientImpl::get_oldest_supported_protocol_version() <= get_current_protocol_version(), "");


/// Information about why a connection (or connection initiation attempt) was
/// terminated. This is used to determinte the delay until the next connection
/// initiation attempt.
enum class ClientImpl::ConnectionTerminationReason {
    resolve_operation_canceled,        ///< Resolve operation (DNS) aborted by client
    resolve_operation_failed,          ///< Failure during resolve operation (DNS)
    connect_operation_canceled,        ///< TCP connect operation aborted by client
    connect_operation_failed,          ///< Failure during TCP connect operation
    closed_voluntarily,                ///< Voluntarily closed after successful connect operation
    premature_end_of_input,            ///< Premature end of input (before ERROR message was received)
    read_or_write_error,               ///< Read/write error after successful TCP connect operation
    http_tunnel_failed,                ///< Failure to establish HTTP tunnel with proxy
    ssl_certificate_rejected,          ///< Client rejected the SSL certificate of the server
    ssl_protocol_violation,            ///< A violation of the SSL protocol
    websocket_protocol_violation,      ///< A violation of the WebSocket protocol
    http_response_says_fatal_error,    ///< Status code in HTTP response says "fatal error"
    http_response_says_nonfatal_error, ///< Status code in HTTP response says "nonfatal error"
    bad_headers_in_http_response,      ///< Missing or bad headers in HTTP response
    sync_protocol_violation,           ///< Client received a bad message from the server
    sync_connect_timeout,              ///< Sync connection was not fully established in time
    server_said_try_again_later,       ///< Client received ERROR message with try_again=yes
    server_said_do_not_reconnect,      ///< Client received ERROR message with try_again=no
    pong_timeout,                      ///< Client did not receive PONG after PING

    /// The application requested a feature that is unavailable in the
    /// negotiated protocol version.
    missing_protocol_feature,
};


/// All use of connection objects, including construction and destruction, must
/// occur on behalf of the event loop thread of the associated client object.
class ClientImpl::Connection final : public util::websocket::Config {
public:
    using connection_ident_type = std::int_fast64_t;
    using ServerEndpoint = std::tuple<ProtocolEnvelope, std::string, port_type, std::string>;

    using SSLVerifyCallback = bool(const std::string& server_address, port_type server_port, const char* pem_data,
                                   size_t pem_size, int preverify_ok, int depth);
    using ProxyConfig = SyncConfig::ProxyConfig;
    using ReconnectInfo = ClientImpl::ReconnectInfo;
    using ReadCompletionHandler = util::websocket::ReadCompletionHandler;
    using WriteCompletionHandler = util::websocket::WriteCompletionHandler;

    util::PrefixLogger logger;

    ClientImpl& get_client() noexcept;
    ReconnectInfo get_reconnect_info() const noexcept;
    ClientProtocol& get_client_protocol() noexcept;

    /// Activate this connection object. No attempt is made to establish a
    /// connection before the connection object is activated.
    void activate();

    /// Activate the specified session.
    ///
    /// Prior to being activated, no messages will be sent or received on behalf
    /// of this session, and the associated Realm file will not be accessed,
    /// i.e., Session::access_realm() will not be called.
    ///
    /// If activation is successful, the connection keeps the session alive
    /// until the application calls initiated_session_deactivation() or until
    /// the application destroys the connection object, whichever comes first.
    void activate_session(std::unique_ptr<Session>);

    /// Initiate the deactivation process which eventually (or immediately)
    /// leads to destruction of this session object.
    ///
    /// IMPORTANT: The session object may get destroyed before this function
    /// returns.
    ///
    /// The deactivation process must be considered initiated even if this
    /// function throws.
    ///
    /// The deactivation process is guaranteed to not be initiated until the
    /// application calls this function. So from the point of view of the
    /// application, after successful activation, a pointer to a session object
    /// remains valid until the application calls
    /// initiate_session_deactivation().
    ///
    /// After the initiation of the deactivation process, the associated Realm
    /// file will no longer be accessed, i.e., access_realm() will not be called
    /// again, and a previously returned reference will also not be accessed
    /// again.
    ///
    /// The initiation of the deactivation process must be preceded by a
    /// successful invocation of activate_session(). It is an error to call
    /// initiate_session_deactivation() twice.
    void initiate_session_deactivation(Session*);

    /// Cancel the reconnect delay for this connection, if one is currently in
    /// effect. If a reconnect delay is not currently in effect, ensure that the
    /// delay before the next reconnection attempt will be canceled. This is
    /// necessary as an apparently established connection, or ongoing connection
    /// attempt can be about to fail for a reason that precedes the invocation
    /// of this function.
    ///
    /// It is an error to call this function before the connection has been
    /// activated.
    void cancel_reconnect_delay();

    /// Returns zero until the HTTP response is received. After that point in
    /// time, it returns the negotiated protocol version, which is based on the
    /// contents of the `Sec-WebSocket-Protocol` header in the HTTP
    /// response. The negotiated protocol version is guaranteed to be greater
    /// than or equal to get_oldest_supported_protocol_version(), and be less
    /// than or equal to get_current_protocol_version().
    int get_negotiated_protocol_version() noexcept;

    // Overriding methods in util::websocket::Config
    util::Logger& websocket_get_logger() noexcept override;
    std::mt19937_64& websocket_get_random() noexcept override;
    void async_read(char*, std::size_t, ReadCompletionHandler) override;
    void async_read_until(char*, std::size_t, char, ReadCompletionHandler) override;
    void async_write(const char*, std::size_t, WriteCompletionHandler) override;
    void websocket_handshake_completion_handler(const util::HTTPHeaders&) override;
    void websocket_read_error_handler(std::error_code) override;
    void websocket_write_error_handler(std::error_code) override;
    void websocket_handshake_error_handler(std::error_code, const util::HTTPHeaders*,
                                           const util::StringView*) override;
    void websocket_protocol_error_handler(std::error_code) override;
    bool websocket_close_message_received(std::error_code error_code, StringData message) override;
    bool websocket_binary_message_received(const char*, std::size_t) override;
    bool websocket_pong_message_received(const char*, std::size_t) override;

    connection_ident_type get_ident() const noexcept;
    const ServerEndpoint& get_server_endpoint() const noexcept;
    ConnectionState get_state() const noexcept;

    void update_connect_info(const std::string& http_request_path_prefix, const std::string& realm_virt_path,
                             const std::string& signed_access_token);

    void resume_active_sessions();

    Connection(ClientImpl&, connection_ident_type, ServerEndpoint, const std::string& authorization_header_name,
               const std::map<std::string, std::string>& custom_http_headers, bool verify_servers_ssl_certificate,
               util::Optional<std::string> ssl_trust_certificate_path, std::function<SSLVerifyCallback>,
               util::Optional<ProxyConfig>, ReconnectInfo);

    ~Connection();

private:
    using ReceivedChangesets = ClientProtocol::ReceivedChangesets;

    template <class H>
    void for_each_active_session(H handler);

    /// \brief Called when the connection becomes idle.
    ///
    /// The connection is considered idle when all of the following conditions
    /// are true:
    ///
    /// - The connection is activated.
    ///
    /// - The connection has no sessions in the Active state.
    ///
    /// - The connection is closed (in the disconnected state).
    ///
    /// From the point of view of this class, an overriding function is allowed
    /// to commit suicide (`delete this`).
    ///
    /// The default implementation of this function does nothing.
    ///
    /// This function is always called by the event loop thread of the
    /// associated client object.
    void on_idle();

    std::string get_http_request_path() const;

    /// The application can override this function to set custom headers. The
    /// default implementation sets no headers.
    void set_http_request_headers(util::HTTPHeaders&);

    void initiate_reconnect_wait();
    void handle_reconnect_wait(std::error_code);
    void initiate_reconnect();
    void initiate_connect_wait();
    void handle_connect_wait(std::error_code);
    void initiate_resolve();
    void handle_resolve(std::error_code, util::network::Endpoint::List);
    void initiate_tcp_connect(util::network::Endpoint::List, std::size_t);
    void handle_tcp_connect(std::error_code, util::network::Endpoint::List, std::size_t);
    void initiate_http_tunnel();
    void handle_http_tunnel(std::error_code);
    void initiate_websocket_or_ssl_handshake();
    void initiate_ssl_handshake();
    void handle_ssl_handshake(std::error_code);
    void initiate_websocket_handshake();
    void handle_connection_established();
    void schedule_urgent_ping();
    void initiate_ping_delay(milliseconds_type now);
    void handle_ping_delay();
    void initiate_pong_timeout();
    void handle_pong_timeout();
    void initiate_write_message(const OutputBuffer&, Session*);
    void handle_write_message();
    void send_next_message();
    void send_ping();
    void initiate_write_ping(const OutputBuffer&);
    void handle_write_ping();
    void handle_message_received(const char* data, std::size_t size);
    void handle_pong_received(const char* data, std::size_t size);
    void initiate_disconnect_wait();
    void handle_disconnect_wait(std::error_code);
    void resolve_error(std::error_code);
    void tcp_connect_error(std::error_code);
    void http_tunnel_error(std::error_code);
    void ssl_handshake_error(std::error_code);
    void read_error(std::error_code);
    void write_error(std::error_code);
    void close_due_to_protocol_error(std::error_code);
    void close_due_to_missing_protocol_feature();
    void close_due_to_client_side_error(std::error_code, bool is_fatal);
    void close_due_to_server_side_error(ProtocolError, StringData message, bool try_again);
    void voluntary_disconnect();
    void involuntary_disconnect(std::error_code ec, bool is_fatal, StringData* custom_message);
    void disconnect(std::error_code ec, bool is_fatal, StringData* custom_message);
    void change_state_to_disconnected() noexcept;

    // These are only called from ClientProtocol class.
    void receive_pong(milliseconds_type timestamp);
    void receive_error_message(int error_code, StringData message, bool try_again, session_ident_type);
    void receive_ident_message(session_ident_type, SaltedFileIdent);
    void receive_download_message(session_ident_type, const SyncProgress&, std::uint_fast64_t downloadable_bytes,
                                  const ReceivedChangesets&);
    void receive_mark_message(session_ident_type, request_ident_type);
    void receive_alloc_message(session_ident_type, file_ident_type file_ident);
    void receive_unbound_message(session_ident_type);
    void handle_protocol_error(ClientProtocol::Error);

    // These are only called from Session class.
    void enlist_to_send(Session*);
    void one_more_active_unsuspended_session();
    void one_less_active_unsuspended_session();

    OutputBuffer& get_output_buffer() noexcept;
    ConnectionTerminationReason determine_connection_termination_reason(std::error_code) noexcept;
    Session* get_session(session_ident_type) const noexcept;
    static bool was_voluntary(ConnectionTerminationReason) noexcept;

    static std::string make_logger_prefix(connection_ident_type);

    void report_connection_state_change(ConnectionState, const SessionErrorInfo*);

    friend ClientProtocol;
    friend class Session;

    ClientImpl& m_client;
    util::Optional<util::network::Resolver> m_resolver;
    util::Optional<util::network::Socket> m_socket;
    util::Optional<util::network::ssl::Context> m_ssl_context;
    util::Optional<util::network::ssl::Stream> m_ssl_stream;
    util::network::ReadAheadBuffer m_read_ahead_buffer;
    util::websocket::Socket m_websocket;
    const ProtocolEnvelope m_protocol_envelope;
    const std::string m_address;
    const port_type m_port;
    const std::string m_http_host; // Contents of `Host:` request header
    const bool m_verify_servers_ssl_certificate;
    const util::Optional<std::string> m_ssl_trust_certificate_path;
    const std::function<SSLVerifyCallback> m_ssl_verify_callback;
    const util::Optional<ProxyConfig> m_proxy_config;
    util::Optional<util::HTTPClient<Connection>> m_proxy_client;
    ReconnectInfo m_reconnect_info;
    int m_negotiated_protocol_version = 0;

    ConnectionState m_state = ConnectionState::disconnected;

    std::size_t m_num_active_unsuspended_sessions = 0;
    std::size_t m_num_active_sessions = 0;
    util::network::Trigger m_on_idle;

    // activate() has been called
    bool m_activated = false;

    // A reconnect delay is in progress
    bool m_reconnect_delay_in_progress = false;

    // Has no meaning when m_reconnect_delay_in_progress is false.
    bool m_nonzero_reconnect_delay = false;

    // A disconnect (linger) delay is in progress. This is for keeping the
    // connection open for a while after there are no more active unsuspended
    // sessions.
    bool m_disconnect_delay_in_progress = false;

    bool m_disconnect_has_occurred = false;

    // A message is currently being sent, i.e., the sending of a message has
    // been initiated, but not yet completed.
    bool m_sending = false;

    bool m_ping_delay_in_progress = false;
    bool m_waiting_for_pong = false;
    bool m_send_ping = false;
    bool m_minimize_next_ping_delay = false;
    bool m_ping_after_scheduled_reset_of_reconnect_info = false;

    // At least one PING message was sent since connection was established
    bool m_ping_sent = false;

    // The timer will be constructed on demand, and will only be destroyed when
    // canceling a reconnect or disconnect delay.
    //
    // It is necessary to destroy and recreate the timer when canceling a wait
    // operation, because the next wait operation might need to be initiated
    // before the completion handler of the previous canceled wait operation
    // starts executing. Such an overlap is not allowed for wait operations on
    // the same timer instance.
    util::Optional<util::network::DeadlineTimer> m_reconnect_disconnect_timer;

    // Timer for connect operation watchdog. For why this timer is optional, see
    // `m_reconnect_disconnect_timer`.
    util::Optional<util::network::DeadlineTimer> m_connect_timer;

    // This timer is used to schedule the sending of PING messages, and as a
    // watchdog for timely reception of PONG messages. For why this timer is
    // optional, see `m_reconnect_disconnect_timer`.
    util::Optional<util::network::DeadlineTimer> m_heartbeat_timer;

    milliseconds_type m_pong_wait_started_at = 0;
    milliseconds_type m_last_ping_sent_at = 0;

    // Round-trip time, in milliseconds, for last PING message for which a PONG
    // message has been received, or zero if no PONG message has been received.
    milliseconds_type m_previous_ping_rtt = 0;

    // Only valid when `m_disconnect_has_occurred` is true.
    milliseconds_type m_disconnect_time = 0;

    // The set of sessions associated with this connection. A session becomes
    // associated with a connection when it is activated.
    std::map<session_ident_type, std::unique_ptr<Session>> m_sessions;

    // A queue of sessions that have enlisted for an opportunity to send a
    // message to the server. Sessions will be served in the order that they
    // enlist. A session is only allowed to occur once in this queue. If the
    // connection is open, and the queue is not empty, and no message is
    // currently being written, the first session is taken out of the queue, and
    // then granted an opportunity to send a message.
    std::deque<Session*> m_sessions_enlisted_to_send;

    Session* m_sending_session = nullptr;

    std::unique_ptr<char[]> m_input_body_buffer;
    OutputBuffer m_output_buffer;

    const connection_ident_type m_ident;
    const ServerEndpoint m_server_endpoint;
    const std::string m_authorization_header_name;
    const std::map<std::string, std::string> m_custom_http_headers;

    std::string m_http_request_path_prefix;
    std::string m_realm_virt_path;
    std::string m_signed_access_token;
};


/// A synchronization session between a local and a remote Realm file.
///
/// All use of session objects, including construction and destruction, must
/// occur on the event loop thread of the associated client object.
class ClientImpl::Session {
public:
    class Config;

    using ReceivedChangesets = ClientProtocol::ReceivedChangesets;
    using IntegrationError = ClientReplication::IntegrationError;

    util::PrefixLogger logger;

    ClientImpl& get_client() noexcept;
    Connection& get_connection() noexcept;
    session_ident_type get_ident() const noexcept;
    SyncProgress get_sync_progress() const noexcept;

    /// Inform this client about new changesets in the history.
    ///
    /// The type of the version specified here is the one that identifies an
    /// entry in the sync history. Whether this is the same as the snapshot
    /// version of the Realm depends on the history implementation.
    ///
    /// The application is supposed to call this function to inform the client
    /// about a new version produced by a transaction that was not performed on
    /// behalf of this client. If the application does not call this function,
    /// the client will not discover and upload new changesets in a timely
    /// manner.
    ///
    /// It is an error to call this function before activation of the session,
    /// or after initiation of deactivation.
    void recognize_sync_version(version_type);

    /// \brief Request notification when all changesets in the local history
    /// have been uploaded to the server.
    ///
    /// When uploading completes, on_upload_completion() will be called by the
    /// thread that processes the event loop (as long as such a thread exists).
    ///
    /// IMPORTANT: on_upload_completion() may get called before
    /// request_upload_completion_notification() returns (reentrant callback).
    ///
    /// If request_upload_completion_notification() is called while a previously
    /// requested completion notification has not yet occurred, the previous
    /// request is canceled and the corresponding notification will never
    /// occur. This ensure that there is no ambiguity about the meaning of each
    /// completion notification.
    ///
    /// The application must be prepared for "spurious" invocations of
    /// on_upload_completion() before the client's first invocation of
    /// request_upload_completion_notification(), or after a previous invocation
    /// of on_upload_completion(), as long as it is before the subsequent
    /// invocation by the client of
    /// request_upload_completion_notification(). This is possible because the
    /// client reserves the right to request upload completion notifications
    /// internally.
    ///
    /// Upload is considered complete when all changesets in the history, that
    /// are supposed to be uploaded, and that precede `current_client_version`,
    /// have been uploaded and acknowledged by the
    /// server. `current_client_version` is generally the version that refers to
    /// the last changeset in the history, but more precisely, it may be any
    /// version between the last version reported by the application through
    /// recognize_sync_version() and the version referring to the last history
    /// entry (both ends inclusive).
    ///
    /// If new changesets are added to the history while a previously requested
    /// completion notification has not yet occurred, it is unspecified whether
    /// the addition of those changesets will cause `current_client_version` to
    /// be bumped or stay fixed, regardless of whether they are advertised via
    /// recognize_sync_version().
    ///
    /// It is an error to call this function before activation of the session,
    /// or after initiation of deactivation.
    void request_upload_completion_notification();

    /// \brief Request notification when all changesets currently avaialble on
    /// the server have been downloaded.
    ///
    /// When downloading completes, on_download_completion() will be called by
    /// the thread that processes the event loop (as long as such a thread
    /// exists).
    ///
    /// If request_download_completion_notification() is called while a
    /// previously requested completion notification has not yet occurred, the
    /// previous request is canceled and the corresponding notification will
    /// never occur. This ensure that there is no ambiguity about the meaning of
    /// each completion notification.
    ///
    /// The application must be prepared for "spurious" invocations of
    /// on_download_completion() before the client's first invocation of
    /// request_download_completion_notification(), or after a previous
    /// invocation of on_download_completion(), as long as it is before the
    /// subsequent invocation by the client of
    /// request_download_completion_notification(). This is possible because the
    /// client reserves the right to request download completion notifications
    /// internally.
    ///
    /// Download is considered complete when all changesets in the server-side
    /// history, that are supposed to be downloaded, and that precede
    /// `current_server_version`, have been downloaded and integrated into the
    /// local history. `current_server_version` is the version that refers to
    /// the last changeset in the server-side history at the time the server
    /// receives the first MARK message that is sent by the client after the
    /// invocation of request_download_completion_notification().
    ///
    /// Every invocation of request_download_completion_notification() will
    /// cause a new MARK message to be sent to the server, to redetermine
    /// `current_server_version`.
    ///
    /// It is an error to call this function before activation of the session,
    /// or after initiation of deactivation.
    void request_download_completion_notification();

    /// \brief Make this client request a new file identifier from the server
    /// for a subordinate client.
    ///
    /// The application is allowed to request additional file identifiers while
    /// it is waiting to receive others.
    ///
    /// The requested file identifiers will be passed back to the application as
    /// they become available. This happens through the virtual callback
    /// function on_subtier_file_ident(), which the application will need to
    /// override. on_subtier_file_ident() will be called once for each requested
    /// identifier as it becomes available.
    ///
    /// The callback function is guaranteed to not be called until after
    /// request_subtier_file_ident() returns (no callback reentrance).
    ///
    /// It is an error to call this function before activation of the session,
    /// or after initiation of deactivation.
    void request_subtier_file_ident();

    /// \brief Announce that a new access token is available.
    ///
    /// By calling this function, the application announces to the session
    /// object that a new access token has been made available, and that it can
    /// be fetched by calling get_signed_access_token().
    ///
    /// This function will not resume a session that has already been suspended
    /// by an error (e.g., `ProtocolError::token_expired`). If the application
    /// wishes to resume such a session, it should follow up with a call to
    /// cancel_resumption_delay().
    ///
    /// Even if the session is not suspended when this function is called, it
    /// may end up becoming suspended before the new access token is delivered
    /// to the server. For example, the prior access token may expire before the
    /// new access token is received by the server, but the ERROR message may
    /// not arrive on the client until after the new token is made available by
    /// the application. This means that the application must be prepared to
    /// receive `ProtocolError::token_expired` after making a new access token
    /// available, even when the new token has not expired. Fortunately, this
    /// should be a rare event, so the application can choose to handle this by
    /// "blindly" renewing the token again, even though such a renewal is
    /// technically redundant.
    ///
    /// FIXME: Improve the implementation of new_access_token_available() such
    /// that there is no risk of getting the session suspended by
    /// `ProtocolError::token_expired` after a new access token has been made
    /// available. Doing this right, requires protocol changes: Add sequence
    /// number to REFRESH messages sent by client, and introduce a REFRESH
    /// response message telling the client that a particular token has been
    /// received by the server.
    ///
    /// IMPORTANT: get_signed_access_token() may get called before
    /// new_access_token_available() returns (reentrant callback).
    ///
    /// It is an error to call this function before activation of the session,
    /// or after initiation of deactivation.
    void new_access_token_available();

    /// If this session is currently suspended, resume it immediately.
    ///
    /// It is an error to call this function before activation of the session,
    /// or after initiation of deactivation.
    void cancel_resumption_delay();

    /// To be used in connection with implementations of
    /// initiate_integrate_changesets().
    ///
    /// This function is thread-safe, but if called from a thread other than the
    /// event loop thread of the associated client object, the specified history
    /// accessor must **not** be the one made available by access_realm().
    bool integrate_changesets(ClientReplication&, const SyncProgress&, std::uint_fast64_t downloadable_bytes,
                              const ReceivedChangesets&, VersionInfo&, IntegrationError&);

    /// To be used in connection with implementations of
    /// initiate_integrate_changesets().
    ///
    /// If \a success is true, the value of \a error does not matter. If \a
    /// success is false, the values of \a client_version and \a
    /// download_progress do not matter.
    ///
    /// It is an error to call this function before activation of the session
    /// (Connection::activate_session()), or after initiation of deactivation
    /// (Connection::initiate_session_deactivation()).
    void on_changesets_integrated(bool success, version_type client_version, DownloadCursor download_progress,
                                  IntegrationError error);

    void on_connection_state_changed(ConnectionState, const SessionErrorInfo*);

    /// The application must ensure that the new session object is either
    /// activated (Connection::activate_session()) or destroyed before the
    /// specified connection object is destroyed.
    ///
    /// The specified transaction reporter (via the config object) is guaranteed
    /// to not be called before activation, and also not after initiation of
    /// deactivation.
    Session(SessionWrapper&, ClientImpl::Connection&, Config);
    ~Session();

private:
    using SyncTransactReporter = ClientReplication::SyncTransactReporter;


    /// Fetch a reference to the remote virtual path of the Realm associated
    /// with this session.
    ///
    /// This function is always called by the event loop thread of the
    /// associated client object.
    ///
    /// This function is guaranteed to not be called before activation, and also
    /// not after initiation of deactivation.
    const std::string& get_virt_path() const noexcept;

    /// Fetch a reference to the signed access token.
    ///
    /// This function is always called by the event loop thread of the
    /// associated client object.
    ///
    /// This function is guaranteed to not be called before activation, and also
    /// not after initiation of deactivation.
    ///
    /// FIXME: For the upstream client of a 2nd tier server it is not ideal that
    /// the admin token needs to be uploaded for every session.
    const std::string& get_signed_access_token() const noexcept;

    const std::string& get_realm_path() const noexcept;
    DB& get_db() const noexcept;

    /// The implementation need only ensure that the returned reference stays valid
    /// until the next invocation of access_realm() on one of the session
    /// objects associated with the same client object.
    ///
    /// This function is always called by the event loop thread of the
    /// associated client object.
    ///
    /// This function is guaranteed to not be called before activation, and also
    /// not after initiation of deactivation.
    ClientReplication& access_realm();

    // client_reset_config() returns the config for client
    // reset. If it returns none, ordinary sync is used. If it returns a
    // Config::ClientReset, the session will be initiated with a state Realm
    // transfer from the server.
    util::Optional<ClientReset>& get_client_reset_config() noexcept;

    /// \brief Initiate the integration of downloaded changesets.
    ///
    /// This function must provide for the passed changesets (if any) to
    /// eventually be integrated, and without unnecessary delay. If no
    /// changesets are passed, the purpose of this function reduces to causing
    /// the current synchronization progress (SyncProgress) to be persisted.
    ///
    /// When all changesets have been integrated, and the synchronization
    /// progress has been persisted, this function must provide for
    /// on_changesets_integrated() to be called without unnecessary delay,
    /// although never after initiation of session deactivation.
    ///
    /// The integration of the specified changesets must happen by means of an
    /// invocation of integrate_changesets(), but not necessarily using the
    /// history accessor made available by access_realm().
    ///
    /// The implementation is allowed, but not obliged to aggregate changesets
    /// from multiple invocations of initiate_integrate_changesets() and pass
    /// them to ClientReplication::integrate_server_changesets() at once.
    ///
    /// The synchronization progress passed to
    /// ClientReplication::integrate_server_changesets() must be obtained
    /// by calling get_sync_progress(), and that call must occur after the last
    /// invocation of initiate_integrate_changesets() whose changesets are
    /// included in what is passed to
    /// ClientReplication::integrate_server_changesets().
    ///
    /// The download cursor passed to on_changesets_integrated() must be
    /// SyncProgress::download of the synchronization progress passed to the
    /// last invocation of
    /// ClientReplication::integrate_server_changesets().
    ///
    /// The default implementation integrates the specified changesets and calls
    /// on_changesets_integrated() immediately (i.e., from the event loop thread
    /// of the associated client object, and before
    /// initiate_integrate_changesets() returns), and via the history accessor
    /// made available by access_realm().
    ///
    /// This function is always called by the event loop thread of the
    /// associated client object, and on_changesets_integrated() must always be
    /// called by that thread too.
    ///
    /// This function is guaranteed to not be called before activation, and also
    /// not after initiation of deactivation.
    void initiate_integrate_changesets(std::uint_fast64_t downloadable_bytes, const ReceivedChangesets&);

    /// See request_upload_completion_notification().
    ///
    /// The default implementation does nothing.
    void on_upload_completion();

    /// See request_download_completion_notification().
    ///
    /// The default implementation does nothing.
    void on_download_completion();

    /// By returning true, this function indicates to the session that the
    /// received file identifier is valid. If the identfier is invald, this
    /// function should return false.
    ///
    /// For more, see request_subtier_file_ident().
    ///
    /// The default implementation returns false, so it must be overridden if
    /// request_subtier_file_ident() is ever called.
    bool on_subtier_file_ident(file_ident_type);

    //@{
    /// These are called as the state of the session changes between
    /// "suspended" and "resumed". The initial state is
    /// always "resumed".
    ///
    /// A switch to the suspended state only happens when an error occurs,
    /// and information about that error is passed to on_suspended().
    ///
    /// The default implementations of these functions do nothing.
    ///
    /// These functions are always called by the event loop thread of the
    /// associated client object.
    ///
    /// These functions are guaranteed to not be called before activation, and also
    /// not after initiation of deactivation.
    void on_suspended(std::error_code ec, StringData message, bool is_fatal);
    void on_resumed();
    //@}

private:
    Connection& m_conn;
    const session_ident_type m_ident;
    SyncTransactReporter* const m_sync_transact_reporter;
    const bool m_disable_upload;
    const bool m_disable_empty_upload;
    const bool m_is_subserver;

    // Session life cycle state:
    //
    //   State          m_deactivation_initiated  m_active_or_deactivating
    //   -----------------------------------------------------------------
    //   Unactivated    false                     false
    //   Active         false                     TRUE
    //   Deactivating   TRUE                      TRUE
    //   Deactivated    TRUE                      false
    //
    // The transition from Deactivating to Deactivated state happens when the
    // unbinding process completes (unbind_process_complete()).
    bool m_deactivation_initiated = false;
    bool m_active_or_deactivating = false;

    bool m_suspended = false;

    // Set to false when a new access token is available and needs to be
    // uploaded to the server. Set to true when uploading of the token has been
    // initiated via a BIND or a REFRESH message.
    bool m_access_token_sent = false;

    // Set to true when download completion is reached. Set to false after a
    // slow reconnect, such that the upload process will become suspended until
    // download completion is reached again.
    bool m_allow_upload = false;

    bool m_upload_completion_notification_requested = false;

    // These are reset when the session is activated, and again whenever the
    // connection is lost or the rebinding process is initiated.
    bool m_enlisted_to_send;
    bool m_bind_message_sent;                   // Sending of BIND message has been initiated
    bool m_ident_message_sent;                  // Sending of IDENT message has been initiated
    bool m_alloc_message_sent;                  // See send_alloc_message()
    bool m_unbind_message_sent;                 // Sending of UNBIND message has been initiated
    bool m_unbind_message_sent_2;               // Sending of UNBIND message has been completed
    bool m_error_message_received;              // Session specific ERROR message received
    bool m_unbound_message_received;            // UNBOUND message received

    // `ident == 0` means unassigned.
    SaltedFileIdent m_client_file_ident = {0, 0};

    // m_client_reset_operation stores state for the lifetime of a client reset
    std::unique_ptr<ClientResetOperation> m_client_reset_operation;

    // The latest sync progress reported by the server via a DOWNLOAD
    // message. See struct SyncProgress for a description. The values stored in
    // `m_progress` either are persisted, or are about to be.
    //
    // Initialized by way of ClientReplication::get_status() at session
    // activation time.
    //
    // `m_progress.upload.client_version` is the client-side sync version
    // produced by the latest local changeset that has been acknowledged as
    // integrated by the server.
    SyncProgress m_progress;

    // In general, the local version produced by the last changeset in the local
    // history. The uploading process will never advance beyond this point. The
    // changeset that produced this version may, or may not contain changes of
    // local origin.
    //
    // It is set to the current version of the local Realm at session activation
    // time (although always zero for the initial empty Realm
    // state). Thereafter, it is generally updated when the application calls
    // recognize_sync_version() and when changesets are received from the server
    // and integrated locally.
    //
    // INVARIANT: m_progress.upload.client_version <= m_last_version_available
    version_type m_last_version_available = 0;

    // The target version for the upload process. When the upload cursor
    // (`m_upload_progress`) reaches `m_upload_target_version`, uploading stops.
    //
    // In general, `m_upload_target_version` follows `m_last_version_available`
    // as it is increased, but in some cases, `m_upload_target_version` will be
    // kept fixed for a while in order to constrain the uploading process.
    //
    // Is set equal to `m_last_version_available` at session activation time.
    //
    // INVARIANT: m_upload_target_version <= m_last_version_available
    version_type m_upload_target_version = 0;

    // In general, this is the position in the history reached while scanning
    // for changesets to be uploaded.
    //
    // Set to `m_progress.upload` at session activation time and whenever the
    // connection to the server is lost. When the connection is established, the
    // scanning for changesets to be uploaded then progresses from there towards
    // `m_upload_target_version`.
    //
    // INVARIANT: m_progress.upload.client_version <= m_upload_progress.client_version
    // INVARIANT: m_upload_progress.client_version <= m_upload_target_version
    UploadCursor m_upload_progress = {0, 0};

    // Set to `m_progress.upload.client_version` at session activation time and
    // whenever the connection to the server is lost. Otherwise it is the
    // version of the latest changeset that has been selected for upload while
    // scanning the history.
    //
    // INVARIANT: m_progress.upload.client_version <= m_last_version_selected_for_upload
    // INVARIANT: m_last_version_selected_for_upload <= m_upload_progress.client_version
    version_type m_last_version_selected_for_upload = 0;

    // Same as `m_progress.download` but is updated only as the progress gets
    // persisted.
    DownloadCursor m_download_progress = {0, 0};

    // Used to implement download completion notifications. Set equal to
    // `m_progress.download.server_version` when a MARK message is received. Set
    // back to zero when `m_download_progress.server_version` becomes greater
    // than, or equal to `m_server_version_at_last_download_mark`. For further
    // details, see check_for_download_completion().
    version_type m_server_version_at_last_download_mark = 0;

    // The serial number to attach to the next download MARK message. A new MARK
    // message will be sent when `m_target_download_mark >
    // m_last_download_mark_sent`. To cause a new MARK message to be sent,
    // simply increment `m_target_download_mark`.
    request_ident_type m_target_download_mark = 0;

    // Set equal to `m_target_download_mark` as the sending of each MARK message
    // is initiated. Must be set equal to `m_last_download_mark_received` when
    // the connection to the server is lost.
    request_ident_type m_last_download_mark_sent = 0;

    // Updated when a MARK message is received. See see
    // check_for_download_completion() for how details on how it participates in
    // the detection of download completion.
    request_ident_type m_last_download_mark_received = 0;

    // Updated when a download completion is detected, to avoid multiple
    // triggerings after reception of a single MARK message. See see
    // check_for_download_completion() for how details on how it participates in
    // the detection of download completion.
    request_ident_type m_last_triggering_download_mark = 0;

    std::int_fast32_t m_num_outstanding_subtier_allocations = 0;

    SessionWrapper& m_wrapper;

    static std::string make_logger_prefix(session_ident_type);

    Session(SessionWrapper& wrapper, Connection&, session_ident_type, Config&&);

    bool do_recognize_sync_version(version_type) noexcept;

    bool have_client_file_ident() const noexcept;

    // The unbinding process completes when both of the following become true:
    //
    //  - The sending of the UNBIND message has been completed
    //    (m_unbind_message_sent_2).
    //
    //  - A session specific ERROR, or the UNBOUND message has been received
    //    (m_error_message_received || m_unbond_message_received).
    //
    // Rebinding (sending of a new BIND message) can only be initiated while the
    // session is in the Active state, and the unbinding process has completed
    // (unbind_process_complete()).
    bool unbind_process_complete() const noexcept;

    void activate();
    void initiate_deactivation();
    void complete_deactivation();
    void connection_established(bool fast_reconnect);
    void connection_lost();
    void send_message();
    void message_sent();
    void send_bind_message();
    void send_ident_message();
    void send_upload_message();
    void send_mark_message();
    void send_alloc_message();
    void send_refresh_message();
    void send_unbind_message();
    std::error_code receive_ident_message(SaltedFileIdent);
    void receive_download_message(const SyncProgress&, std::uint_fast64_t downloadable_bytes,
                                  const ReceivedChangesets&);
    std::error_code receive_mark_message(request_ident_type);
    std::error_code receive_alloc_message(file_ident_type file_ident);
    std::error_code receive_unbound_message();
    std::error_code receive_error_message(int error_code, StringData message, bool try_again);

    void initiate_rebind();
    void reset_protocol_state() noexcept;
    void ensure_enlisted_to_send();
    void enlist_to_send();
    void update_progress(const SyncProgress&);
    bool check_received_sync_progress(const SyncProgress&) noexcept;
    bool check_received_sync_progress(const SyncProgress&, int&) noexcept;
    void check_for_upload_completion();
    void check_for_download_completion();

    friend class Connection;
};


/// See Client::Session for the meaning of the individual properties
/// (other than `sync_transact_reporter`).
class ClientImpl::Session::Config {
public:
    SyncTransactReporter* sync_transact_reporter = nullptr;
    bool disable_upload = false;
    bool disable_empty_upload = false;
    bool is_subserver = false;
};


// Implementation

inline void ClientImpl::report_event_loop_metrics(std::function<EventLoopMetricsHandler> handler)
{
    m_service.report_event_loop_metrics(std::move(handler)); // Throws
}

inline const std::string& ClientImpl::get_user_agent_string() const noexcept
{
    return m_user_agent_string;
}

inline auto ClientImpl::get_reconnect_mode() const noexcept -> ReconnectMode
{
    return m_reconnect_mode;
}

inline bool ClientImpl::is_dry_run() const noexcept
{
    return m_dry_run;
}

inline bool ClientImpl::get_tcp_no_delay() const noexcept
{
    return m_tcp_no_delay;
}

inline util::network::Service& ClientImpl::get_service() noexcept
{
    return m_service;
}

inline std::mt19937_64& ClientImpl::get_random() noexcept
{
    return m_random;
}

inline auto ClientImpl::get_next_session_ident() noexcept -> session_ident_type
{
    return ++m_prev_session_ident;
}

inline void ClientImpl::ReconnectInfo::reset() noexcept
{
    m_reason = util::none;
    m_time_point = 0;
    m_delay = 0;
    m_scheduled_reset = false;
}

inline ClientImpl& ClientImpl::Connection::get_client() noexcept
{
    return m_client;
}

inline ConnectionState ClientImpl::Connection::get_state() const noexcept
{
    return m_state;
}

inline auto ClientImpl::Connection::get_reconnect_info() const noexcept -> ReconnectInfo
{
    return m_reconnect_info;
}

inline auto ClientImpl::Connection::get_client_protocol() noexcept -> ClientProtocol&
{
    return m_client.m_client_protocol;
}

inline int ClientImpl::Connection::get_negotiated_protocol_version() noexcept
{
    return m_negotiated_protocol_version;
}

inline ClientImpl::Connection::~Connection() {}

template <class H>
void ClientImpl::Connection::for_each_active_session(H handler)
{
    for (auto& p : m_sessions) {
        Session& sess = *p.second;
        if (!sess.m_deactivation_initiated)
            handler(sess); // Throws
    }
}

inline void ClientImpl::Connection::voluntary_disconnect()
{
    REALM_ASSERT(m_reconnect_info.m_reason && was_voluntary(*m_reconnect_info.m_reason));
    std::error_code ec = ClientError::connection_closed;
    bool is_fatal = false;
    StringData* custom_message = nullptr;
    disconnect(ec, is_fatal, custom_message); // Throws
}

inline void ClientImpl::Connection::involuntary_disconnect(std::error_code ec, bool is_fatal,
                                                           StringData* custom_message)
{
    REALM_ASSERT(m_reconnect_info.m_reason && !was_voluntary(*m_reconnect_info.m_reason));
    disconnect(ec, is_fatal, custom_message); // Throws
}

inline void ClientImpl::Connection::change_state_to_disconnected() noexcept
{
    REALM_ASSERT(m_state != ConnectionState::disconnected);
    m_state = ConnectionState::disconnected;

    if (m_num_active_sessions == 0)
        m_on_idle.trigger();

    REALM_ASSERT(!m_reconnect_delay_in_progress);
    if (m_disconnect_delay_in_progress) {
        m_reconnect_disconnect_timer = util::none;
        m_disconnect_delay_in_progress = false;
    }
}

inline void ClientImpl::Connection::one_more_active_unsuspended_session()
{
    if (m_num_active_unsuspended_sessions++ != 0)
        return;
    // Rose from zero to one
    if (m_state == ConnectionState::disconnected && !m_reconnect_delay_in_progress && m_activated)
        initiate_reconnect(); // Throws
}

inline void ClientImpl::Connection::one_less_active_unsuspended_session()
{
    if (--m_num_active_unsuspended_sessions != 0)
        return;
    // Dropped from one to zero
    if (m_state != ConnectionState::disconnected)
        initiate_disconnect_wait(); // Throws
}

// Sessions, and the connection, should get the output_buffer and insert a message,
// after which they call initiate_write_output_buffer(Session* sess).
inline auto ClientImpl::Connection::get_output_buffer() noexcept -> OutputBuffer&
{
    m_output_buffer.reset();
    return m_output_buffer;
}

inline auto ClientImpl::Connection::get_session(session_ident_type ident) const noexcept -> Session*
{
    auto i = m_sessions.find(ident);
    bool found = (i != m_sessions.end());
    return found ? i->second.get() : nullptr;
}

inline bool ClientImpl::Connection::was_voluntary(ConnectionTerminationReason reason) noexcept
{
    switch (reason) {
        case ConnectionTerminationReason::resolve_operation_canceled:
        case ConnectionTerminationReason::connect_operation_canceled:
        case ConnectionTerminationReason::closed_voluntarily:
            return true;
        case ConnectionTerminationReason::resolve_operation_failed:
        case ConnectionTerminationReason::connect_operation_failed:
        case ConnectionTerminationReason::premature_end_of_input:
        case ConnectionTerminationReason::read_or_write_error:
        case ConnectionTerminationReason::ssl_certificate_rejected:
        case ConnectionTerminationReason::ssl_protocol_violation:
        case ConnectionTerminationReason::websocket_protocol_violation:
        case ConnectionTerminationReason::http_response_says_fatal_error:
        case ConnectionTerminationReason::http_response_says_nonfatal_error:
        case ConnectionTerminationReason::bad_headers_in_http_response:
        case ConnectionTerminationReason::sync_protocol_violation:
        case ConnectionTerminationReason::sync_connect_timeout:
        case ConnectionTerminationReason::server_said_try_again_later:
        case ConnectionTerminationReason::server_said_do_not_reconnect:
        case ConnectionTerminationReason::pong_timeout:
        case ConnectionTerminationReason::missing_protocol_feature:
        case ConnectionTerminationReason::http_tunnel_failed:
            break;
    }
    return false;
}

inline ClientImpl& ClientImpl::Session::get_client() noexcept
{
    return m_conn.get_client();
}

inline auto ClientImpl::Session::get_connection() noexcept -> Connection&
{
    return m_conn;
}

inline auto ClientImpl::Session::get_ident() const noexcept -> session_ident_type
{
    return m_ident;
}

inline auto ClientImpl::Session::get_sync_progress() const noexcept -> SyncProgress
{
    return m_progress;
}

inline void ClientImpl::Session::recognize_sync_version(version_type version)
{
    // Life cycle state must be Active
    REALM_ASSERT(m_active_or_deactivating);
    REALM_ASSERT(!m_deactivation_initiated);

    bool resume_upload = do_recognize_sync_version(version);
    if (REALM_LIKELY(resume_upload)) {
        // Since the deactivation process has not been initiated, the UNBIND
        // message cannot have been sent unless an ERROR message was received.
        REALM_ASSERT(m_error_message_received || !m_unbind_message_sent);
        if (m_ident_message_sent && !m_error_message_received)
            ensure_enlisted_to_send(); // Throws
    }
}

inline void ClientImpl::Session::request_upload_completion_notification()
{
    // Life cycle state must be Active
    REALM_ASSERT(m_active_or_deactivating);
    REALM_ASSERT(!m_deactivation_initiated);

    m_upload_completion_notification_requested = true;
    check_for_upload_completion(); // Throws
}

inline void ClientImpl::Session::request_download_completion_notification()
{
    // Life cycle state must be Active
    REALM_ASSERT(m_active_or_deactivating);
    REALM_ASSERT(!m_deactivation_initiated);

    ++m_target_download_mark;

    // Since the deactivation process has not been initiated, the UNBIND message
    // cannot have been sent unless an ERROR message was received.
    REALM_ASSERT(m_error_message_received || !m_unbind_message_sent);
    if (m_ident_message_sent && !m_error_message_received)
        ensure_enlisted_to_send(); // Throws
}

inline void ClientImpl::Session::request_subtier_file_ident()
{
    // Life cycle state must be Active
    REALM_ASSERT(m_active_or_deactivating);
    REALM_ASSERT(!m_deactivation_initiated);

    bool was_zero = (m_num_outstanding_subtier_allocations == 0);
    ++m_num_outstanding_subtier_allocations;

    // Since the deactivation process has not been initiated, the UNBIND message
    // cannot have been sent unless an ERROR message was received.
    REALM_ASSERT(m_error_message_received || !m_unbind_message_sent);
    if (was_zero && m_ident_message_sent && !m_error_message_received) {
        if (!m_alloc_message_sent)
            ensure_enlisted_to_send(); // Throws
    }
}

inline void ClientImpl::Session::new_access_token_available()
{
    // Life cycle state must be Active
    REALM_ASSERT(m_active_or_deactivating);
    REALM_ASSERT(!m_deactivation_initiated);

    m_access_token_sent = false;

    // Since the deactivation process has not been initiated, the UNBIND message
    // cannot have been sent unless an ERROR message was received.
    REALM_ASSERT(m_error_message_received || !m_unbind_message_sent);
    if (m_bind_message_sent && !m_error_message_received)
        ensure_enlisted_to_send(); // Throws
}

inline ClientImpl::Session::Session(SessionWrapper& wrapper, Connection& conn, Config config)
    : Session{wrapper, conn, conn.get_client().get_next_session_ident(), std::move(config)} // Throws
{
}

inline ClientImpl::Session::Session(SessionWrapper& wrapper, Connection& conn, session_ident_type ident,
                                    Config&& config)
    : logger{make_logger_prefix(ident), conn.logger} // Throws
    , m_conn{conn}
    , m_ident{ident}
    , m_sync_transact_reporter{config.sync_transact_reporter}
    , m_disable_upload{config.disable_upload}
    , m_disable_empty_upload{config.disable_empty_upload}
    , m_is_subserver{config.is_subserver}
    , m_wrapper{wrapper}
{
    if (get_client().m_disable_upload_activation_delay)
        m_allow_upload = true;
}

inline bool ClientImpl::Session::do_recognize_sync_version(version_type version) noexcept
{
    if (REALM_LIKELY(version > m_last_version_available)) {
        m_last_version_available = version;
        m_upload_target_version = version;
        return true;
    }
    return false;
}

inline bool ClientImpl::Session::have_client_file_ident() const noexcept
{
    return (m_client_file_ident.ident != 0);
}

inline bool ClientImpl::Session::unbind_process_complete() const noexcept
{
    return (m_unbind_message_sent_2 && (m_error_message_received || m_unbound_message_received));
}

inline void ClientImpl::Session::connection_established(bool fast_reconnect)
{
    // This function must only be called for sessions in the Active state.
    REALM_ASSERT(!m_deactivation_initiated);
    REALM_ASSERT(m_active_or_deactivating);


    if (!fast_reconnect && !get_client().m_disable_upload_activation_delay) {
        // Disallow immediate activation of the upload process, even if download
        // completion was reached during an earlier period of connectivity.
        m_allow_upload = false;
    }

    if (!m_allow_upload) {
        // Request download completion notification
        ++m_target_download_mark;
    }

    if (!m_suspended) {
        // Ready to send BIND message
        enlist_to_send(); // Throws
    }
}

// The caller (Connection) must discard the session if the session has become
// deactivated upon return.
inline void ClientImpl::Session::connection_lost()
{
    REALM_ASSERT(m_active_or_deactivating);
    // If the deactivation process has been initiated, it can now be immediately
    // completed.
    if (m_deactivation_initiated) {
        // Life cycle state is Deactivating
        complete_deactivation(); // Throws
        // Life cycle state is now Deactivated
        return;
    }
    reset_protocol_state();
}

// The caller (Connection) must discard the session if the session has become
// deactivated upon return.
inline void ClientImpl::Session::message_sent()
{
    // Note that it is possible for this function to get called after the client
    // has received a message sent by the server in reposnse to the message that
    // the client has just finished sending.

    // Session life cycle state is Active or Deactivating
    REALM_ASSERT(m_active_or_deactivating);

    // No message will be sent after the UNBIND message
    REALM_ASSERT(!m_unbind_message_sent_2);

    if (m_unbind_message_sent) {
        REALM_ASSERT(!m_enlisted_to_send);

        // If the sending of the UNBIND message has been initiated, this must be
        // the time when the sending of that message completes.
        m_unbind_message_sent_2 = true;

        // Detect the completion of the unbinding process
        if (m_error_message_received || m_unbound_message_received) {
            // If the deactivation process has been initiated, it can now be
            // immediately completed.
            if (m_deactivation_initiated) {
                // Life cycle state is Deactivating
                complete_deactivation(); // Throws
                // Life cycle state is now Deactivated
                return;
            }

            // The session is still in the Active state, so initiate the
            // rebinding process if the session is no longer suspended.
            if (!m_suspended)
                initiate_rebind(); // Throws
        }
    }
}

inline void ClientImpl::Session::initiate_rebind()
{
    // Life cycle state must be Active
    REALM_ASSERT(m_active_or_deactivating);
    REALM_ASSERT(!m_deactivation_initiated);

    REALM_ASSERT(!m_suspended);
    REALM_ASSERT(!m_enlisted_to_send);

    reset_protocol_state();

    // Ready to send BIND message
    enlist_to_send(); // Throws
}

inline void ClientImpl::Session::reset_protocol_state() noexcept
{
    // clang-format off
    m_enlisted_to_send                    = false;
    m_bind_message_sent                   = false;
    m_ident_message_sent = false;
    m_alloc_message_sent = false;
    m_unbind_message_sent = false;
    m_unbind_message_sent_2 = false;
    m_error_message_received = false;
    m_unbound_message_received = false;

    m_upload_progress = m_progress.upload;
    m_last_version_selected_for_upload = m_upload_progress.client_version;
    m_last_download_mark_sent          = m_last_download_mark_received;
    // clang-format on
}

inline void ClientImpl::Session::ensure_enlisted_to_send()
{
    if (!m_enlisted_to_send)
        enlist_to_send(); // Throws
}

// This function will never "commit suicide" despite the fact that it may
// involve an invocation of send_message(), which in certain cases can lead to
// the completion of the deactivation process, and if that did happen, it would
// cause Connection::send_next_message() to destroy this session, but it does
// not happen.
//
// If the session is already in the Deactivating state, send_message() will
// complete the deactivation process immediately when, and only when the BIND
// message has not already been sent.
//
// Note however, that this function gets called when the establishment of the
// connection completes, but at that time, the session cannot be in the
// Deactivating state, because until the BIND message is sent, the deactivation
// process will complete immediately. So the first invocation of this function
// after establishemnt of the connection will not commit suicide.
//
// Note then, that the session will stay enlisted to send, until it gets to send
// the BIND message, and since the and enlist_to_send() must not be called while
// the session is enlisted, the next invocation of this function will be after
// the BIND message has been sent, but then the deactivation process will no
// longer be completed by send_message().
inline void ClientImpl::Session::enlist_to_send()
{
    REALM_ASSERT(m_active_or_deactivating);
    REALM_ASSERT(!m_unbind_message_sent);
    REALM_ASSERT(!m_enlisted_to_send);
    m_enlisted_to_send = true;
    m_conn.enlist_to_send(this); // Throws
}

inline bool ClientImpl::Session::check_received_sync_progress(const SyncProgress& progress) noexcept
{
    int error_code = 0; // Dummy
    return check_received_sync_progress(progress, error_code);
}

} // namespace sync
} // namespace realm

#endif // REALM_NOINST_CLIENT_IMPL_BASE_HPP
