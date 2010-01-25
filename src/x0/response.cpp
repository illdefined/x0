/* <x0/response.cpp>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 *
 * (c) 2009 Chrisitan Parpart <trapni@gentoo.org>
 */

#include <x0/response.hpp>
#include <x0/server.hpp>
#include <x0/strutils.hpp>
#include <x0/types.hpp>

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <string>
#include <strings.h>						// strcasecmp

namespace x0 {

using boost::algorithm::iequals;

char response::status_codes[512][4];

response::~response()
{
	//DEBUG("~response(%p, conn=%p)", this, connection_.get());
	delete request_;
}

response& response::operator+=(const x0::response_header& value)
{
	headers.push_back(value);

	return *this;
}

response& response::operator*=(const x0::response_header& in)
{
	for (std::vector<x0::response_header>::iterator i = headers.begin(); i != headers.end(); ++i)
	{
#if 0
		if (iequals(i->name, in.name))
#else
		if (strcasecmp(i->name.c_str(), in.name.c_str()) == 0)
#endif
		{
			i->value = in.value;
			return *this;
		}
	}

	headers.push_back(in);
	return *this;
}


bool response::has_header(const std::string& name) const
{
	for (std::vector<x0::response_header>::const_iterator i = headers.begin(); i != headers.end(); ++i)
	{
#if 0
		if (iequals(i->name, name))
#else
		if (strcasecmp(i->name.c_str(), name.c_str()) == 0)
#endif
		{
			return true;
		}
	}

	return false;
}

std::string response::header(const std::string& name) const
{
	for (std::vector<x0::response_header>::const_iterator i = headers.begin(); i != headers.end(); ++i)
	{
		if (strcasecmp(i->name.c_str(), name.c_str()) == 0)
		{
			return i->value;
		}
	}

	return std::string();
}

const std::string& response::header(const std::string& name, const std::string& value)
{
	for (std::vector<x0::response_header>::iterator i = headers.begin(); i != headers.end(); ++i)
	{
		if (strcasecmp(i->name.c_str(), name.c_str()) == 0)
		{
			return i->value = value;
		}
	}

	headers.push_back(x0::response_header(name, value));
	return headers[headers.size() - 1].value;
}

/** checks wether given code MUST NOT have a response body. */
static inline bool content_forbidden(int code)
{
	switch (code)
	{
		case response::continue_:
		case response::switching_protocols:
		case response::no_content:
		case response::reset_content:
		case response::not_modified:
			return true;
		default:
			return false;
	}
}

composite_buffer response::serialize()
{
	composite_buffer buffers;

	if (!serializing_)
	{
		if (!status)
		{
			status = response::ok;
		}

		if (content.empty() && !content_forbidden(status) && equals(request_->method, "GET"))
		{
			const char *codeStr = status_cstr(status);
			char buf[1024];

			int nwritten = snprintf(buf, sizeof(buf),
				"<html>"
				"<head><title>%s</title></head>"
				"<body><h1>%d %s</h1></body>"
				"</html>",
				codeStr, status(), codeStr
			);
			write(std::string(buf, 0, nwritten));

			header("Content-Length", boost::lexical_cast<std::string>(content_length()));
			header("Content-Type", "text/html");
		}
		else if (!has_header("Content-Type"))
		{
			*this += x0::response_header("Content-Type", "text/plain");
		}

		if (!has_header("Content-Length") && !content_forbidden(status))
		{
			header("Connection", "closed");
		}
		else if (!has_header("Connection"))
		{
			if (iequals(request_->header("Connection"), "keep-alive"))
			{
				header("Connection", "keep-alive");
			}
			else
			{
				header("Connection", "closed");
			}
		}

		// log request/response
		connection_->server().request_done(*request_, *this);

		// post-response hook
		connection_->server().post_process(*request_, *this);

		if (request_->supports_protocol(1, 1))
			buffers.push_back("HTTP/1.1 ");
		else if (request_->supports_protocol(1, 0))
			buffers.push_back("HTTP/1.0 ");
		else
			buffers.push_back("HTTP/0.9 ");

		buffers.push_back(status_codes[status]);
		buffers.push_back(' ');
		buffers.push_back(status_cstr(status));
		buffers.push_back("\r\n");

		for (std::size_t i = 0; i < headers.size(); ++i)
		{
			const x0::response_header& h = headers[i];

			buffers.push_back(h.name.data(), h.name.size());
			buffers.push_back(": ");
			buffers.push_back(h.value.data(), h.value.size());
			buffers.push_back("\r\n");
		}

		buffers.push_back("\r\n");

		serializing_ = true;
	}

	buffers.push_back(content);

	return buffers;
}

response::response(connection_ptr connection, x0::request *request, int _status) :
	connection_(connection),
	request_(request),
	serializing_(false),
	status(_status),
	headers()
{
	//DEBUG("response(%p, conn=%p)", this, connection_.get());

	*this += x0::response_header("Date", connection_->server().now().http_str());
	*this += x0::response_header("Server", connection_->server().tag());
}

const char *response::status_cstr(int value)
{
	switch (value)
	{
		case 200: return "Ok";
		case 201: return "Created";
		case 202: return "Accepted";
		case 204: return "No Content";
		case 206: return "Partial Content";
		case 300: return "Multiple Choices";
		case 301: return "Moved Permanently";
		case 302: return "Moved Temporarily";
		case 304: return "Not Modified";
		case 400: return "Bad Request";
		case 401: return "Unauthorized";
		case 403: return "Forbidden";
		case 404: return "Not Found";
		case 416: return "Requested Range Not Satisfiable";
		case 500: return "Internal Server Error";
		case 501: return "Not_Implemented";
		case 502: return "Bad Gateway";
		case 503: return "Service Unavailable";
		default: return "";
	}
}

std::string response::status_str(int value)
{
	return std::string(status_cstr(value));
}

void response::transmitted(const asio::error_code& ec)
{
	//DEBUG("response(%p).transmitted(%s)", this, ec.message().c_str());

	if (!ec)
	{
		if (strcasecmp(header("Connection").c_str(), "keep-alive") == 0)
		{
			connection_->resume();
		}
	}

	delete this;
}

void response::initialize()
{
	// pre-compute string representations of status codes for use in response serialization
	for (std::size_t i = 0; i < sizeof(status_codes) / sizeof(*status_codes); ++i)
		snprintf(status_codes[i], sizeof(*status_codes), "%03ld", i);
}

} // namespace x0
