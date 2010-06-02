/* <x0/connection.hpp>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 *
 * (c) 2009 Chrisitan Parpart <trapni@gentoo.org>
 */

#ifndef x0_connection_hpp
#define x0_connection_hpp (1)

#include <x0/http/message_processor.hpp>
#include <x0/http/server.hpp>
#include <x0/io/sink.hpp>
#include <x0/io/source.hpp>
#include <x0/io/async_writer.hpp>
#include <x0/buffer.hpp>
#include <x0/property.hpp>
#include <x0/types.hpp>
#include <x0/api.hpp>
#include <x0/sysconfig.h>

#if defined(WITH_SSL)
#	include <gnutls/gnutls.h>
#endif

#include <functional>
#include <memory>
#include <string>
#include <ev++.h>
#include <netinet/in.h> // sockaddr_in

namespace x0 {

//! \addtogroup core
//@{

class connection_sink;
class request;

/**
 * \brief represents an HTTP connection handling incoming requests.
 */
class connection :
	public message_processor
{
public:
	connection& operator=(const connection&) = delete;
	connection(const connection&) = delete;

	/**
	 * creates an HTTP connection object.
	 * \param srv a ptr to the server object this connection belongs to.
	 */
	explicit connection(x0::listener& listener);

	~connection();

	void start();
	void close();

	value_property<bool> secure;				//!< true if this is a secure (HTTPS) connection, false otherwise.

	int handle() const;							//!< Retrieves a reference to the connection socket.
	x0::server& server();						//!< Retrieves a reference to the server instance.

	std::string remote_ip() const;				//!< Retrieves the IP address of the remote end point (client).
	int remote_port() const;					//!< Retrieves the TCP port numer of the remote end point (client).

	std::string local_ip() const;
	int local_port() const;

	template<typename CompletionHandler>
	void on_write_ready(CompletionHandler callback);

	void stop_write();

	const x0::listener& listener() const;

#if defined(WITH_SSL)
	bool ssl_enabled() const;
#endif

private:
	friend class request;
	friend class response;
	friend class x0::listener;
	friend class connection_sink;

	virtual void message_begin(buffer_ref&& method, buffer_ref&& entity, int version_major, int version_minor);
	virtual void message_header(buffer_ref&& name, buffer_ref&& value);
	virtual bool message_header_done();
	virtual bool message_content(buffer_ref&& chunk);
	virtual bool message_end();

	bool is_closed() const;
	void resume(bool finish);

	void start_read();
	void resume_read();
	void handle_read();

	void start_write();
	void resume_write();
	void handle_write();

	void process();
	void check_request_body();
	void async_write(const source_ptr& buffer, const completion_handler_type& handler);
	void io(ev::io& w, int revents);

#if defined(WITH_CONNECTION_TIMEOUTS)
	void timeout(ev::timer& watcher, int revents);
#endif

#if defined(WITH_SSL)
	void ssl_initialize();
	bool ssl_handshake();
#endif

	struct ::ev_loop *loop() const;

public:
	std::map<plugin *, custom_data_ptr> custom_data;

private:
	x0::listener& listener_;
	x0::server& server_;				//!< server object owning this connection

	int socket_;						//!< underlying communication socket
	sockaddr_in6 saddr_;

	mutable std::string remote_ip_;		//!< internal cache to client ip
	mutable int remote_port_;			//!< internal cache to client port

	// HTTP request
	buffer buffer_;						//!< buffer for incoming data.
	std::size_t next_offset_;			//!< number of bytes in buffer_ successfully processed already.
	int request_count_;					//!< number of requests already processed within this connection.
	request *request_;					//!< currently parsed http request, may be NULL
	response *response_;				//!< currently processed response object, may be NULL

	enum {
		INVALID,
		READING,
		WRITING,
	} io_state_;

#if defined(WITH_SSL)
	gnutls_session_t ssl_session_;		//!< SSL (GnuTLS) session handle
	bool handshaking_;
#endif

	ev::io watcher_;

#if defined(WITH_CONNECTION_TIMEOUTS)
	ev::timer timer_;					//!< deadline timer for detecting read/write timeouts.
#endif

#if !defined(NDEBUG)
	ev::tstamp ctime_;
#endif

	std::function<void(connection *)> write_some;
	std::function<void(connection *)> read_some;
};

// {{{ inlines
inline struct ::ev_loop* connection::loop() const
{
	return server_.loop();
}

inline int connection::handle() const
{
	return socket_;
}

inline server& connection::server()
{
	return server_;
}

/** write something into the connection stream.
 * \param buffer the buffer of bytes to be written into the connection.
 * \param handler the completion handler to invoke once the buffer has been either fully written or an error occured.
 */
inline void connection::async_write(const source_ptr& buffer, const completion_handler_type& handler)
{
	check_request_body();
	x0::async_write(this, buffer, handler);
}

template<typename CompletionHandler>
inline void connection::on_write_ready(CompletionHandler callback)
{
	write_some = callback;
	start_write();
}

inline const x0::listener& connection::listener() const
{
	return listener_;
}

/** tests whether connection::close() was invoked already.
 */
inline bool connection::is_closed() const
{
	return socket_ < 0;
}
// }}}

//@}

} // namespace x0

#endif