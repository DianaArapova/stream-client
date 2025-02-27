#pragma once

#include <random>

namespace {

template <typename T>
void shuffle_vector(std::vector<T>& v)
{
    static std::random_device r_device;
    static std::mt19937 r_generator(r_device());
    std::shuffle(v.begin(), v.end(), r_generator);
}

} // anonymous namespace

namespace stream_client {
namespace connector {

template <typename Stream>
base_connector<Stream>::base_connector(const std::string& host, const std::string& port,
                                       time_duration_type resolve_timeout, time_duration_type connect_timeout,
                                       time_duration_type operation_timeout,
                                       ::stream_client::resolver::ip_family ip_family, resolve_flags_type resolve_flags)
    : host_(host)
    , port_(port)
    , resolve_timeout_(resolve_timeout)
    , connect_timeout_(connect_timeout)
    , operation_timeout_(operation_timeout)
    , resolver_(host_, port_, resolve_timeout_, std::move(ip_family), std::move(resolve_flags))
{
    resolve_done_ = false;
    resolve_needed_ = true;
    resolving_thread_running_.store(true, std::memory_order_release);
    resolving_thread_ = std::thread([this]() { this->resolve_routine(); });
}

template <typename Stream>
base_connector<Stream>::~base_connector()
{
    resolving_thread_running_.store(false, std::memory_order_release);
    if (resolving_thread_.joinable()) {
        resolving_thread_.join();
    }
}

template <typename Stream>
std::unique_ptr<typename base_connector<Stream>::stream_type>
base_connector<Stream>::new_session(boost::system::error_code& ec, const time_point_type& deadline)
{
    std::unique_lock<std::timed_mutex> resolve_done_lk(resolve_done_mutex_, std::defer_lock);
    if (!resolve_done_lk.try_lock_until(deadline)) {
        // failed to lock resolve_done_mutex_ within deadline
        auto resolve_ec = get_resolve_error();
        ec = resolve_ec ? std::move(resolve_ec) : boost::asio::error::timed_out;
        return nullptr;
    }
    if (resolve_done_ == false &&
        !resolve_done_cv_.wait_until(resolve_done_lk, deadline, [this] { return resolve_done_; })) {
        // faield to wait for endpoints resolution
        auto resolve_ec = get_resolve_error();
        ec = resolve_ec ? std::move(resolve_ec) : boost::asio::error::timed_out;
        return nullptr;
    }
    // unlock owned resolve_done_mutex_ to release other new_session() calls while we are connecting
    resolve_done_lk.unlock();

    auto endpoints = get_endpoints();
    shuffle_vector(endpoints);
    for (const auto& peer : endpoints) {
        try {
            return connect_until(peer, deadline);
        } catch (const boost::system::system_error& err) {
            ec = err.code();
            break;
        }
    }
    if (!ec) {
        // endpoints may be empty because of resolve error
        ec = get_resolve_error();
    }
    // if failed to connect trigger resolving thread to update endpoints
    notify_resolve_needed();
    return nullptr;
}

template <typename Stream>
void base_connector<Stream>::resolve_routine()
{
    static const auto lock_timeout = std::chrono::milliseconds(100);

    while (resolving_thread_running_.load(std::memory_order_acquire)) {
        std::unique_lock<std::timed_mutex> resolve_needed_lk(resolve_needed_mutex_, std::defer_lock);
        if (!resolve_needed_lk.try_lock_for(lock_timeout) ||
            !resolve_needed_cv_.wait_for(resolve_needed_lk, lock_timeout, [this] { return resolve_needed_; })) {
            continue;
        }
        // at this point we owe locked resolve_needed_mutex_

        boost::system::error_code resolve_ec;
        resolver_endpoint_iterator_type new_endpoints = resolver_.resolve(resolve_ec);
        set_resolve_error(resolve_ec);
        if (resolve_ec) {
            continue;
        }

        resolve_needed_ = false;
        update_endpoints(std::move(new_endpoints));
        notify_resolve_done();
    }
}

// connect_until specialization for tcp_client
template <>
inline std::unique_ptr<::stream_client::tcp_client>
base_connector<::stream_client::tcp_client>::connect_until(const endpoint_type& peer_endpoint,
                                                           const time_point_type& until_time) const
{
    const time_duration_type connect_timeout{until_time - clock_type::now()};
    return std::make_unique<::stream_client::tcp_client>(peer_endpoint, connect_timeout, operation_timeout_);
}

// connect_until specialization for udp_client
template <>
inline std::unique_ptr<::stream_client::udp_client>
base_connector<::stream_client::udp_client>::connect_until(const endpoint_type& peer_endpoint,
                                                           const time_point_type& until_time) const
{
    const time_duration_type connect_timeout{until_time - clock_type::now()};
    return std::make_unique<::stream_client::udp_client>(peer_endpoint, connect_timeout, operation_timeout_);
}

// connect_until specialization for ssl::ssl_client
template <>
inline std::unique_ptr<::stream_client::ssl::ssl_client>
base_connector<::stream_client::ssl::ssl_client>::connect_until(const endpoint_type& peer_endpoint,
                                                                const time_point_type& until_time) const
{
    const time_duration_type connect_timeout{until_time - clock_type::now()};
    return std::make_unique<::stream_client::ssl::ssl_client>(peer_endpoint, connect_timeout, operation_timeout_,
                                                              host_);
}

// connect_until specialization for http_client
template <>
inline std::unique_ptr<::stream_client::http::http_client>
base_connector<::stream_client::http::http_client>::connect_until(const endpoint_type& peer_endpoint,
                                                                  const time_point_type& until_time) const
{
    const time_duration_type connect_timeout{until_time - clock_type::now()};
    return std::make_unique<::stream_client::http::http_client>(peer_endpoint, connect_timeout, operation_timeout_);
}

// connect_until specialization for https_client
template <>
inline std::unique_ptr<::stream_client::http::https_client>
base_connector<::stream_client::http::https_client>::connect_until(const endpoint_type& peer_endpoint,
                                                                   const time_point_type& until_time) const
{
    const time_duration_type connect_timeout{until_time - clock_type::now()};
    return std::make_unique<::stream_client::http::https_client>(peer_endpoint, connect_timeout, operation_timeout_,
                                                                 host_);
}

} // namespace connector
} // namespace stream_client
