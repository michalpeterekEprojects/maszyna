#include "network/backend/asio.h"
#include "sn_utils.h"
#include "Logs.h"

network::tcp::connection::connection(asio::io_context &io_ctx, bool client)
    : network::connection(client), m_socket(io_ctx)
{
	m_header_buffer.resize(8);
}

void network::tcp::connection::disconnect()
{
	network::connection::disconnect();
	m_socket.close();
}

void network::tcp::connection::send_data(std::shared_ptr<std::string> buffer)
{
	asio::async_write(m_socket, asio::buffer(*buffer.get()), std::bind(&connection::handle_send, this, buffer,
	                                                            std::placeholders::_1,
	                                                            std::placeholders::_2));
}

void network::tcp::connection::connected()
{
	network::connection::connected();
	read_header();
}

void network::tcp::connection::read_header()
{
	asio::async_read(m_socket, asio::buffer(m_header_buffer),
	                    std::bind(&connection::handle_header, this,
	                              std::placeholders::_1, std::placeholders::_2));
}

void network::tcp::connection::handle_send(
        std::shared_ptr<std::string> buf, const asio::error_code &err, size_t bytes_transferred)
{

}

void network::tcp::connection::handle_header(const asio::error_code &err, size_t bytes_transferred)
{
	std::istringstream header(m_header_buffer);
	if (m_header_buffer.size() != bytes_transferred) {
		disconnect();
		return;
	}

	uint32_t sig = sn_utils::ld_uint32(header);
	if (sig != NETWORK_MAGIC) {
		disconnect();
		return;
	}

	uint32_t len = sn_utils::ld_uint32(header);
	m_body_buffer.resize(len);
	if (len > MAX_MSG_SIZE) {
		disconnect();
		return;
	}

	asio::async_read(m_socket, asio::buffer(m_body_buffer),
	                 std::bind(&connection::handle_data, this, std::placeholders::_1, std::placeholders::_2));
}

void network::tcp::connection::handle_data(const asio::error_code &err, size_t bytes_transferred)
{
	if (m_body_buffer.size() != bytes_transferred) {
		disconnect();
		return;
	}

	std::istringstream stream(m_body_buffer);
	std::shared_ptr<message> msg = deserialize_message(stream);
	if (message_handler)
		message_handler(*msg);

	read_header();
}

void network::tcp::connection::send_message(const message &msg)
{
	std::ostringstream stream;
	sn_utils::ls_uint32(stream, NETWORK_MAGIC);
	sn_utils::ls_uint32(stream, 0);

	serialize_message(msg, stream);

	size_t size = (size_t)stream.tellp() - 8;
	if (size > MAX_MSG_SIZE) {
		ErrorLog("net: message too big", logtype::net);
		return;
	}
	stream.seekp(4, std::ios_base::beg);
	sn_utils::ls_uint32(stream, size);

	stream.flush();

	std::shared_ptr<std::string> buf = std::make_shared<std::string>(stream.str());
	send_data(buf);
}

// -----------------

network::tcp::server::server(asio::io_context &io_ctx, const std::string &host, uint32_t port)
    : m_acceptor(io_ctx)
{
	auto endpoint = asio::ip::tcp::endpoint(asio::ip::address::from_string(host), port);
	m_acceptor.open(endpoint.protocol());
	m_acceptor.set_option(asio::socket_base::reuse_address(true));
	m_acceptor.set_option(asio::ip::tcp::no_delay(true));
	m_acceptor.bind(endpoint);
	m_acceptor.listen(10);

	accept_conn();
}

void network::tcp::server::accept_conn()
{
	std::shared_ptr<connection> conn = std::make_shared<connection>(m_acceptor.get_executor().context());
	conn->set_handler(std::bind(&server::handle_message, this, std::ref(*conn.get()), std::placeholders::_1));

	m_acceptor.async_accept(conn->m_socket, std::bind(&server::handle_accept, this, conn, std::placeholders::_1));
}

void network::tcp::server::handle_accept(std::shared_ptr<connection> conn, const asio::error_code &err)
{
	if (!err)
	{
		clients.emplace_back(conn);
		conn->connected();
	}
	else
	{
		WriteLog(std::string("net: failed to accept client: " + err.message()), logtype::net);
	}

	accept_conn();
}

// ------------------

network::tcp::client::client(asio::io_context &io_ctx, const std::string &host, uint32_t port)
{
	std::shared_ptr<connection> conn = std::make_shared<connection>(io_ctx, true);
	conn->set_handler(std::bind(&client::handle_message, this, std::ref(*conn.get()), std::placeholders::_1));

	asio::ip::tcp::endpoint endpoint(
	            asio::ip::address::from_string(host), port);
	conn->m_socket.open(endpoint.protocol());
	conn->m_socket.set_option(asio::ip::tcp::no_delay(true));
	conn->m_socket.async_connect(endpoint,
	                    std::bind(&client::handle_accept, this, std::placeholders::_1));

	this->conn = conn;
}

void network::tcp::client::handle_accept(const asio::error_code &err)
{
	if (!err)
	{
		conn->connected();
	}
	else
	{
		WriteLog(std::string("net: failed to connect: " + err.message()), logtype::net);
	}
}
