/*
 * COPYRIGHT (C) 2017, zhllxt
 *
 * author   : zhllxt
 * qq       : 37792738
 * email    : 37792738@qq.com
 * 
 */

#ifndef __ASIO2_UDP_SENDER_IMPL_HPP__
#define __ASIO2_UDP_SENDER_IMPL_HPP__

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
#pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <memory>
#include <thread>
#include <atomic>

#include <boost/asio.hpp>

#include <asio2/util/pool.hpp>
#include <asio2/util/helper.hpp>

#include <asio2/base/sender_impl.hpp>
#include <asio2/base/io_service_pool.hpp>
#include <asio2/base/listener_mgr.hpp>

namespace asio2
{

	template<class _pool_t>
	class udp_sender_impl : public sender_impl
	{
	public:

		typedef _pool_t pool_t;

		/**
		 * @construct
		 */
		explicit udp_sender_impl(
			std::shared_ptr<listener_mgr> listener_mgr_ptr,
			std::shared_ptr<url_parser> url_parser_ptr
		)
			: sender_impl(listener_mgr_ptr, url_parser_ptr)
		{
		}

		/**
		 * @destruct
		 */
		virtual ~udp_sender_impl()
		{
		}

		/**
		 * @function : start the sender.you must call the stop function before application exit,otherwise will cause crash.
		 * @return   : true  - start successed 
		 *             false - start failed
		 */
		virtual bool start() override
		{
			try
			{
				// check if started and not stoped
				if (is_start())
				{
					assert(false);
					return false;
				}

				// first call base class start function
				if (!sender_impl::start())
					return false;

				// reset the state to the default
				m_fire_close_is_called.clear(std::memory_order_release);

				// init service pool size 
				m_io_service_pool_evt_ptr = std::make_shared<io_service_pool>(_get_io_service_pool_size());
				m_io_service_pool_msg_ptr = std::make_shared<io_service_pool>(_get_io_service_pool_size());

				// init recv buf pool 
				m_recv_buf_pool_ptr = std::make_shared<pool_t>(_get_pool_buffer_size());

				// startup the io service thread 
				m_io_service_thread_evt_ptr = std::make_shared<std::thread>(std::bind(&io_service_pool::run, m_io_service_pool_evt_ptr));
				m_io_service_thread_msg_ptr = std::make_shared<std::thread>(std::bind(&io_service_pool::run, m_io_service_pool_msg_ptr));

				// start connect
				return _start_connect();
			}
			catch (boost::system::system_error & e)
			{
				set_last_error(e.code().value(), e.what());
			}
			catch (std::exception & e)
			{
				set_last_error(DEFAULT_EXCEPTION_CODE, e.what());
				PRINT_EXCEPTION;
			}

			return false;
		}

		/**
		 * @function : stop the sender
		 */
		virtual void stop() override
		{
			bool is_start = this->is_start();

			m_stop_is_called = true;

			// call listen socket's close function to notify the _handle_accept function response with error > 0 ,then the listen socket 
			// can get notify to exit
			if (m_socket_ptr && m_socket_ptr->is_open())
			{
				// close the socket by post a event
				if (is_start)
				{
					// use promise and future to wait for the async post finished.
					std::promise<void> promise_send;

					// first wait for all send pending complete
					m_evt_send_strand_ptr->post([this, &promise_send]()
					{
						// do nothing ,just make sure the send pending is last executed
						auto_promise<void> ap(promise_send);
					});

					promise_send.get_future().wait();

					// use promise and future to wait for the async post finished.
					std::promise<void> promise_recv;

					// asio don't allow operate the same socket in multi thread,if you close socket in one thread and another thread is 
					// calling socket's async_read... function,it will crash.so we must care for operate the socket.when need close the
					// socket ,we use the strand to post a event,make sure the socket's close operation is in the same thread.
					m_evt_recv_strand_ptr->post([this, &promise_recv]()
					{
						boost::system::error_code ec;

						m_socket_ptr->shutdown(boost::asio::socket_base::shutdown_both, ec);
						if (ec)
							set_last_error(ec.value(), ec.message());

						m_socket_ptr->close(ec);
						if (ec)
							set_last_error(ec.value(), ec.message());

						auto_promise<void> ap(promise_recv);
					});

					// wait util the socket is closed completed
					// must wait for the socket closed completed,otherwise when use m_evt_strand_ptr post a event to close the socket,
					// before socket closed ,the event thread join is returned already,and it will cause memory leaks

					// wait for the async task finish,when finished,the socket must be closed already.
					promise_recv.get_future().wait();
				}
				// close the socket directly
				else
				{
					boost::system::error_code ec;

					m_socket_ptr->shutdown(boost::asio::socket_base::shutdown_both, ec);
					if (ec)
						set_last_error(ec.value(), ec.message());

					m_socket_ptr->close(ec);
					if (ec)
						set_last_error(ec.value(), ec.message());
				}
			}

			if (m_io_service_pool_evt_ptr)
				m_io_service_pool_evt_ptr->stop();
			if (m_io_service_pool_msg_ptr)
				m_io_service_pool_msg_ptr->stop();

			if (m_io_service_thread_evt_ptr && m_io_service_thread_evt_ptr->joinable())
				m_io_service_thread_evt_ptr->join();
			if (m_io_service_thread_msg_ptr && m_io_service_thread_msg_ptr->joinable())
				m_io_service_thread_msg_ptr->join();

			// release the buffer pool malloced memory 
			if (m_recv_buf_pool_ptr)
				m_recv_buf_pool_ptr->destroy();

			m_stop_is_called = false;
		}

		/**
		 * @function : whether the sender is started
		 */
		virtual bool is_start()
		{
			return (
				!m_stop_is_called &&
				m_evt_send_ioservice_ptr &&
				m_evt_recv_ioservice_ptr &&
				m_msg_send_ioservice_ptr &&
				m_msg_recv_ioservice_ptr &&
				m_evt_send_strand_ptr &&
				m_evt_recv_strand_ptr &&
				m_msg_send_strand_ptr &&
				m_msg_recv_strand_ptr &&
				(m_socket_ptr && m_socket_ptr->is_open()) &&
				(m_io_service_thread_evt_ptr && m_io_service_thread_evt_ptr->joinable()) &&
				(m_io_service_thread_msg_ptr && m_io_service_thread_msg_ptr->joinable())
				);
		}

		/**
		 * @function : send data
		 */
		virtual bool send(std::string ip, unsigned short port, std::shared_ptr<uint8_t> send_buf_ptr, std::size_t len)
		{
			try
			{
				if (is_start() && !ip.empty())
				{
					boost::asio::ip::udp::endpoint recver_endpoint(
						boost::asio::ip::address::from_string(ip),
						port);

					// note : can't use m_io_service_msg to post event,because can't operate socket in multi thread.
					// must use strand.post to send data.why we should do it like this ? see udp_session._post_send.
					m_evt_send_strand_ptr->post(std::bind(&udp_sender_impl::_post_send,
						std::static_pointer_cast<udp_sender_impl>(shared_from_this()),
						recver_endpoint, send_buf_ptr, len));
					return true;
				}
			}
			catch (boost::system::system_error & e)
			{
				set_last_error(e.code().value(), e.what());
			}
			return false;
		}

	public:

		/**
		 * @function : get the socket shared_ptr
		 */
		inline std::shared_ptr<boost::asio::ip::udp::socket> get_socket_ptr()
		{
			return m_socket_ptr;
		}

		/**
		 * @function : get the local address
		 */
		virtual std::string get_local_address() override
		{
			try
			{
				if (m_socket_ptr && m_socket_ptr->is_open())
				{
					auto endpoint = m_socket_ptr->local_endpoint();
					return endpoint.address().to_string();
				}
			}
			catch (boost::system::system_error & e)
			{
				set_last_error(e.code().value(), e.what());
			}
			return "";
		}

		/**
		 * @function : get the local port
		 */
		virtual unsigned short get_local_port() override
		{
			try
			{
				if (m_socket_ptr && m_socket_ptr->is_open())
				{
					auto endpoint = m_socket_ptr->local_endpoint();
					return endpoint.port();
				}
			}
			catch (boost::system::system_error & e)
			{
				set_last_error(e.code().value(), e.what());
			}
			return 0;
		}

		/**
		 * @function : get the remote address
		 */
		virtual std::string get_remote_address() override
		{
			try
			{
				if (m_socket_ptr && m_socket_ptr->is_open())
				{
					auto endpoint = m_socket_ptr->remote_endpoint();
					return endpoint.address().to_string();
				}
			}
			catch (boost::system::system_error & e)
			{
				set_last_error(e.code().value(), e.what());
			}
			return "";
		}

		/**
		 * @function : get the remote port
		 */
		virtual unsigned short get_remote_port() override
		{
			try
			{
				if (m_socket_ptr && m_socket_ptr->is_open())
				{
					auto endpoint = m_socket_ptr->remote_endpoint();
					return endpoint.port();
				}
			}
			catch (boost::system::system_error & e)
			{
				set_last_error(e.code().value(), e.what());
			}
			return 0;
		}

		/**
		 * @function : get socket's recv buffer size
		 */
		virtual int get_recv_buffer_size() override
		{
			try
			{
				if (m_socket_ptr && m_socket_ptr->is_open())
				{
					boost::asio::socket_base::receive_buffer_size option;
					m_socket_ptr->get_option(option);
					return option.value();
				}
			}
			catch (boost::system::system_error & e)
			{
				set_last_error(e.code().value(), e.what());
			}
			return 0;
		}

		/**
		 * @function : set socket's recv buffer size.
		 *             when packet lost rate is high,you can set the recv buffer size to a big value to avoid it.
		 */
		virtual bool set_recv_buffer_size(int size) override
		{
			try
			{
				if (m_socket_ptr && m_socket_ptr->is_open())
				{
					boost::asio::socket_base::receive_buffer_size option(size);
					m_socket_ptr->set_option(option);
					return true;
				}
			}
			catch (boost::system::system_error & e)
			{
				set_last_error(e.code().value(), e.what());
			}
			return false;
		}

		/**
		 * @function : get socket's send buffer size
		 */
		virtual int get_send_buffer_size() override
		{
			try
			{
				if (m_socket_ptr && m_socket_ptr->is_open())
				{
					boost::asio::socket_base::send_buffer_size option;
					m_socket_ptr->get_option(option);
					return option.value();
				}
			}
			catch (boost::system::system_error & e)
			{
				set_last_error(e.code().value(), e.what());
			}
			return 0;
		}

		/**
		 * @function : set socket's send buffer size
		 */
		virtual bool set_send_buffer_size(int size) override
		{
			try
			{
				if (m_socket_ptr && m_socket_ptr->is_open())
				{
					boost::asio::socket_base::send_buffer_size option(size);
					m_socket_ptr->set_option(option);
					return true;
				}
			}
			catch (boost::system::system_error & e)
			{
				set_last_error(e.code().value(), e.what());
			}
			return false;
		}

	protected:

		virtual std::size_t _get_io_service_pool_size() override
		{
			// get io_service_pool_size from the url
			std::size_t io_service_pool_size = 2;
			std::string str_io_service_pool_size = m_url_parser_ptr->get_param_value("io_service_pool_size");
			if (!str_io_service_pool_size.empty())
				io_service_pool_size = static_cast<std::size_t>(std::atoi(str_io_service_pool_size.c_str()));
			if (io_service_pool_size == 0)
				io_service_pool_size = 2;
			return io_service_pool_size;
		}

		virtual std::size_t _get_pool_buffer_size() override
		{
			// get pool_buffer_size from the url
			std::size_t pool_buffer_size = 576; // udp packet size best not more than 576,that is, MTU size
			std::string str_pool_buffer_size = m_url_parser_ptr->get_param_value("pool_buffer_size");
			if (!str_pool_buffer_size.empty())
			{
				pool_buffer_size = static_cast<std::size_t>(std::atoi(str_pool_buffer_size.c_str()));
				if (str_pool_buffer_size.find_last_of('k') != std::string::npos)
					pool_buffer_size *= 1024;
				else if (str_pool_buffer_size.find_last_of('m') != std::string::npos)
					pool_buffer_size *= 1024 * 1024;
			}
			if (pool_buffer_size < 16)
				pool_buffer_size = 576;
			return pool_buffer_size;
		}

		virtual bool _start_connect()
		{
			try
			{
				m_async_notify = (m_url_parser_ptr->get_param_value("notify_mode") == "async");

				m_evt_send_ioservice_ptr = m_io_service_pool_evt_ptr->get_io_service_ptr();
				m_evt_recv_ioservice_ptr = m_io_service_pool_evt_ptr->get_io_service_ptr();
				m_msg_send_ioservice_ptr = m_io_service_pool_msg_ptr->get_io_service_ptr();
				m_msg_recv_ioservice_ptr = m_io_service_pool_msg_ptr->get_io_service_ptr();

				m_evt_send_strand_ptr = std::make_shared <boost::asio::io_service::strand>(*m_evt_send_ioservice_ptr);
				m_evt_recv_strand_ptr = std::make_shared <boost::asio::io_service::strand>(*m_evt_recv_ioservice_ptr);
				m_msg_send_strand_ptr = std::make_shared <boost::asio::io_service::strand>(*m_msg_send_ioservice_ptr);
				m_msg_recv_strand_ptr = std::make_shared <boost::asio::io_service::strand>(*m_msg_recv_ioservice_ptr);

				boost::asio::ip::udp::endpoint bind_endpoint(
					boost::asio::ip::address::from_string(m_url_parser_ptr->get_ip()),
					static_cast<unsigned short>(std::atoi(m_url_parser_ptr->get_port().c_str())));

				// socket contructor function with endpoint param will automic call open and bind.
				m_socket_ptr = std::make_shared<boost::asio::ip::udp::socket>(
					*m_evt_recv_ioservice_ptr, bind_endpoint);

				//m_socket_ptr->open(endpoint.protocol());
				//m_socket_ptr->bind(endpoint);

				_post_recv();

				return (m_socket_ptr && m_socket_ptr->is_open());
			}
			catch (boost::system::system_error & e)
			{
				set_last_error(e.code().value(), e.what());
			}

			return false;
		}

		virtual void _post_recv()
		{
			if (is_start())
			{
				// every times post recv event,we get the recv buffer from the buffer pool
				std::shared_ptr<uint8_t> recv_buf_ptr = m_recv_buf_pool_ptr->get();

				try
				{
					m_socket_ptr->async_receive_from(
						boost::asio::buffer(recv_buf_ptr.get(), m_recv_buf_pool_ptr->get_requested_size()),
						m_sender_endpoint,
						m_evt_recv_strand_ptr->wrap(std::bind(&udp_sender_impl::_handle_recv, std::static_pointer_cast<udp_sender_impl>(shared_from_this()),
							std::placeholders::_1,
							std::placeholders::_2,
							recv_buf_ptr
						)));
				}
				catch (std::exception & e)
				{
					set_last_error(DEFAULT_EXCEPTION_CODE, e.what());
					PRINT_EXCEPTION;
				}
			}
		}

		virtual void _handle_recv(const boost::system::error_code& ec, std::size_t bytes_recvd, std::shared_ptr<uint8_t> recv_buf_ptr)
		{
			set_last_error(ec.value());

			if (!ec)
			{
				if (bytes_recvd == 0)
				{
					// recvd data len is 0,may be heartbeat packet.
				}
				else if (bytes_recvd > 0)
				{
					// recvd data 
				}

				_fire_recv(m_sender_endpoint, recv_buf_ptr, bytes_recvd);

				_post_recv();
			}
			else
			{
				// close this sender
				_fire_close(ec.value());

				//// may be user pressed the CTRL + C to exit application.
				//if (ec == boost::asio::error::operation_aborted)
				//	return;

				//// if user call stop to stop client,the socket is closed,then _handle_recv will be called,and with error,so when appear error,we check 
				//// the socket status,if closed,don't call _post_recv again.
				//if (!m_socket_ptr || !m_socket_ptr->is_open())
				//	return;
			}
		}

		virtual void _post_send(boost::asio::ip::udp::endpoint recver_endpoint, std::shared_ptr<uint8_t> send_buf_ptr, std::size_t len)
		{
			if (is_start())
			{
				boost::system::error_code ec;
				size_t bytes_sent = m_socket_ptr->send_to(boost::asio::buffer(send_buf_ptr.get(), len), recver_endpoint, 0, ec);
				set_last_error(ec.value());

				_fire_send(recver_endpoint, send_buf_ptr, bytes_sent, ec.value());

				if (ec)
				{
					PRINT_EXCEPTION;

					_fire_close(ec.value());
				}
			}
		}

		virtual void _fire_recv(boost::asio::ip::udp::endpoint sender_endpoint, std::shared_ptr<uint8_t> recv_buf_ptr, std::size_t bytes_recvd)
		{
			try
			{
				if (is_start() && std::static_pointer_cast<sender_listener_mgr>(m_listener_mgr_ptr)->is_recv_listener_exist())
				{
					if (m_async_notify)
					{
						// when recv one msg,we don't handle it in this socket thread,instead we handle it in another thread by io_service.post function.
						m_msg_recv_strand_ptr->post(std::bind(&udp_sender_impl::_do_fire_recv,
							std::static_pointer_cast<udp_sender_impl>(shared_from_this()),
							sender_endpoint, recv_buf_ptr, bytes_recvd));
					}
					else
					{
						_do_fire_recv(sender_endpoint, recv_buf_ptr, bytes_recvd);
					}
				}
			}
			catch (std::exception &) {}
		}

		virtual void _do_fire_recv(boost::asio::ip::udp::endpoint & sender_endpoint, std::shared_ptr<uint8_t> recv_buf_ptr, std::size_t bytes_recvd)
		{
			try
			{
				std::static_pointer_cast<sender_listener_mgr>(m_listener_mgr_ptr)->notify_recv(sender_endpoint.address().to_string(), sender_endpoint.port(), recv_buf_ptr, bytes_recvd);
			}
			catch (std::exception &) {}
		}

		virtual void _fire_send(boost::asio::ip::udp::endpoint recver_endpoint, std::shared_ptr<uint8_t> send_buf_ptr, std::size_t bytes_sent, int error)
		{
			try
			{
				if (is_start() && std::static_pointer_cast<sender_listener_mgr>(m_listener_mgr_ptr)->is_send_listener_exist())
				{
					if (m_async_notify)
					{
						m_msg_send_strand_ptr->post(std::bind(&udp_sender_impl::_do_fire_send,
							std::static_pointer_cast<udp_sender_impl>(shared_from_this()),
							recver_endpoint, send_buf_ptr, bytes_sent, error));
					}
					else
					{
						_do_fire_send(recver_endpoint, send_buf_ptr, bytes_sent, error);
					}
				}
			}
			catch (std::exception &) {}
		}

		virtual void _do_fire_send(boost::asio::ip::udp::endpoint & recver_endpoint, std::shared_ptr<uint8_t> send_buf_ptr, std::size_t bytes_sent, int error)
		{
			try
			{
				std::static_pointer_cast<sender_listener_mgr>(m_listener_mgr_ptr)->notify_send(recver_endpoint.address().to_string(), recver_endpoint.port(), send_buf_ptr, bytes_sent, error);
			}
			catch (std::exception &) {}
		}

		virtual void _fire_close(int error)
		{
			if (!m_fire_close_is_called.test_and_set(std::memory_order_acquire))
				_do_fire_close(error);
		}

		virtual void _do_fire_close(int error)
		{
			try
			{
				std::dynamic_pointer_cast<sender_listener_mgr>(m_listener_mgr_ptr)->notify_close(error);
			}
			catch (std::exception &) {}
		}

	protected:

		std::shared_ptr<boost::asio::ip::udp::socket> m_socket_ptr;

		/// the m_io_service_pool_evt thread for socket event
		std::shared_ptr<std::thread> m_io_service_thread_evt_ptr;

		/// the m_io_service_pool_msg for msg handle
		std::shared_ptr<std::thread> m_io_service_thread_msg_ptr;

		/// the io_service_pool for socket event
		io_service_pool_ptr m_io_service_pool_evt_ptr;

		/// the io_service_pool for msg handle
		io_service_pool_ptr m_io_service_pool_msg_ptr;

		/// recv buffer pool for every session
		std::shared_ptr<pool_t> m_recv_buf_pool_ptr;

		/// notify mode,async or sync
		bool m_async_notify = false;

		/// use to check whether the user call session stop function
		volatile bool m_stop_is_called = false;

		/// use to avoid call _fire_close twice
		std::atomic_flag m_fire_close_is_called = ATOMIC_FLAG_INIT;

		/// endpoint for udp 
		boost::asio::ip::udp::endpoint m_sender_endpoint;

	};
}

#endif // !__ASIO2_UDP_SENDER_IMPL_HPP__