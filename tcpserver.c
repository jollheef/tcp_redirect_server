/**
 * @file tcpserver.c
 * @author Михаил Клементьев < jollheef <AT> riseup.net >
 * @date Март 2015
 * @license GPLv3
 * @brief tcp сервер
 *
 * TCP сервер, перенаправляющий поток с пользователя на приложение.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

/**
 * Проверить значение на -1 и в случае, если это так
 * вывести сообщение об ошибке и выйти с EXIT_FAILURE.
 *
 * @param[in] e возвращаемое значение для проверки.
 * @param[in] m сообщение об ошибке.
 * @return EXIT_FAILURE в случае ошибки или продолжение
 *         исполнения в случае корректного значения.
 */
#ifdef DEBUG
#define CHECK_ERRNO(e, m)				\
	perror(m); if (-1 == e) { exit(EXIT_FAILURE); }
#define DBG_printf(fmt, arg...) printf(fmt, ##arg)
#else
#define CHECK_ERRNO(e, m)				\
	if (-1 == e) { perror(m); exit(EXIT_FAILURE); }
#define DBG_printf(fmt, arg...)
#endif

/**
 * Вывести сообщение вида "TRACE: Имя файла имя_функции:строка".
 */
#ifdef TRACE
#undef TRACE
#define TRACE (printf("TRACE: %s %s:%d\n", __FILE__, __FUNCTION__, __LINE__))
#else
#define TRACE
#endif

/**
 * Количество ожидающих обработки соединений.
 */
#define LISTEN_BACKLOG (100)

/**
 * Количество одновременно обслуживаемых соединений.
 */
#define HANDLE_CONNS_COUNT (100)

/**
 * Счетчик обслуживаемых в данный момент соединений.
 */
int connections = 0;

/**
 * Блокировка для счетчика обслуживаемых в данный момент соединений.
 */
pthread_mutex_t connections_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * Счетчик соединений.
 */
long long int connections_count = 0;

/**
 * Блокировка во время инициализации соединения.
 * Для корректного закрытия в этом случае.
 */
pthread_mutex_t init_connection_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * TCP порт, на котором принимает соединение сервис.
 */
#define LISTEN_PORT (50006)

#define IN
#define OUT

/**
 * Структура, описывающая соединение.
 */
struct connection_vars_t {
	long long int n;	   /* Номер соединения */
	int sockfd;		   /* Дескриптор сокета клиента */
	struct sockaddr_in addr; /* Описание адрес:порт соединения клиента.*/
};

/**
 * Вывести структуру, описывающую соединение, в стандартный вывод.
 *
 * @param[in] conn указатель на структуру.
 */
void dump_connection_vars (IN struct connection_vars_t* conn)
{
	printf ("Адрес структуры: %p\t|", conn);
	printf ("Номер соединения: %lld\t|", conn->n);
	printf ("Дескриптор сокета клиента: %d\t|", conn->sockfd);
	printf ("Описание соединения клиента: %s:%d\n",
		inet_ntoa (conn->addr.sin_addr), ntohs (conn->addr.sin_port));
}

/**
 * Массив открытых на данный момент соединений.
 */
struct connection_vars_t* current_connections[HANDLE_CONNS_COUNT];

/**
 * Вывод состояния сохраненных соединений в стандартный вывод.
 */
void dump_connections (void)
{
	for (int i = 0; i < HANDLE_CONNS_COUNT; ++i) {
		if (NULL != current_connections[i]) {
			TRACE;
			dump_connection_vars (current_connections[i]);
		}
	}
}

/**
 * Сохранить соединение в массиве соединений.
 *
 * @param[in] conn -- указатель на структуру соединения.
 */
int save_conn (IN struct connection_vars_t* conn)
{
	int status = 1;

	for (int i = 0; i < HANDLE_CONNS_COUNT; ++i) {
		if (NULL == current_connections[i]) {
			TRACE;
#ifdef DEBUG
			dump_connection_vars (conn);
#endif
			current_connections[i] = conn;

			connections += 1;

			status = 0;

			break;
		}
	}

	return status;
}

/**
 * Удалить соединение из массива соединений.
 *
 * @param[in] conn -- указатель на структуру соединения.
 */
void remove_conn (IN struct connection_vars_t* conn)
{
	TRACE;

	for (int i = 0; i < HANDLE_CONNS_COUNT; ++i) {
		if (NULL != current_connections[i]) {
			TRACE;

			if (conn->n == current_connections[i]->n) {
				TRACE;
#ifdef DEBUG
				dump_connection_vars (conn);
#endif
				current_connections[i] = NULL;
				free (conn);
				connections -= 1;
				return;
			}
		}
	}

	fprintf (stderr, "Connection not found\n");
}

/**
 * Получить количество свободных мест в массиве соединений.
 */
int get_free_places (void)
{
	int places = 0;

	for (int i = 0; i < HANDLE_CONNS_COUNT; ++i) {
		if (NULL == current_connections[i]) {
			++places;
		}
	}

	return places;
}


/**
 * Закрыть и очистить память для всех текущих соединений.
 */
void free_all_conn (void)
{
	for (int i = 0; i < HANDLE_CONNS_COUNT; ++i) {
		if (NULL == current_connections[i]) {
			continue;
		}

		struct connection_vars_t* connection = current_connections[i];

		TRACE;

#ifdef DEBUG
		dump_connection_vars (connection);

#endif
		int status = shutdown (connection->sockfd, SHUT_RDWR);

		CHECK_ERRNO (status, "Shutdown connection");

		status = close (connection->sockfd);

		CHECK_ERRNO (status, "Close connection");

		connections -= 1;

		free (current_connections[i]);

		current_connections[i] = NULL;
	}
}

/**
 * Обработчик соединения.
 *
 * @param[in] connection описание соединения.
 * @return не используется.
 */
void* handler (IN struct connection_vars_t* connection)
{
	/* Поток должен запуститься только после окончания инициализации */
	pthread_mutex_lock (&init_connection_lock);
	pthread_mutex_unlock (&init_connection_lock);

	TRACE;

	//sleep (1);			/* TTTTTTTTTTTTTTTTTTT */

	TRACE;

#ifdef DEBUG
	dump_connection_vars (connection);
#endif

	int status = shutdown (connection->sockfd, SHUT_RDWR);

	CHECK_ERRNO (status, "Shutdown connection");

	TRACE;

	status = close (connection->sockfd);

	CHECK_ERRNO (status, "Close connection");

	pthread_mutex_lock (&connections_lock);
	remove_conn (connection);
	pthread_mutex_unlock (&connections_lock);

	return NULL;
}

/**
 * Цикл обработки входящий соединений.
 *
 * @param[in] server_sockfd сокет, принимающий входящие соединения.
 * @param[in] handler обработчик соединения.
 * @param статус завершения.
 */
int
connections_loop (IN int server_sockfd,
		  IN void * (*_handler) (struct connection_vars_t* ))
{
	int status = 0;

	while (true) {
		TRACE;

#ifdef DEBUG
		pthread_mutex_lock (&connections_lock);
		printf ("Connections: %d\n", connections);
		printf ("Free places: %d\n", get_free_places() );

		if (HANDLE_CONNS_COUNT != (connections + get_free_places() ) ) {
			fprintf (stderr, "Something went wrong\n");

			TRACE;
			dump_connections ();

			pthread_mutex_unlock (&connections_lock);
			return -EINVAL;
		}

		pthread_mutex_unlock (&connections_lock);
#endif

		struct sockaddr_in client_addr;

		memset (&client_addr, 0, sizeof (struct sockaddr_in) );

		socklen_t client_addr_size = sizeof (struct sockaddr_in);

		int client_sockfd = accept (
			server_sockfd,
			(struct sockaddr*) &client_addr,
			&client_addr_size
                        );

		CHECK_ERRNO (client_sockfd, "Accept connection");

		struct timespec req;
		req.tv_sec = 0;
		req.tv_nsec = 10000000; /* 0.01 секунды */
		
		int isLimit = false;

		do {
			TRACE;
			pthread_mutex_lock (&connections_lock);
			isLimit = connections > HANDLE_CONNS_COUNT - 1;
			pthread_mutex_unlock (&connections_lock);

			nanosleep(&req, NULL);
		}
		while (isLimit);

		pthread_mutex_lock (&init_connection_lock);

		++connections_count;

		DBG_printf ("Connect: %lld\n", connections_count);

		pthread_t client_thread;

		struct connection_vars_t* connection =
			calloc (1, sizeof (struct connection_vars_t) );

		if (NULL == connection) {
			TRACE;
			fprintf (stderr, "Allocation failed\n");
			exit (EXIT_FAILURE);
		}

		TRACE;

		connection->sockfd = client_sockfd;
		connection->addr = client_addr;
		connection->n = connections_count;

		TRACE;

		status = pthread_create (
			&client_thread,
			NULL,
			(void * (*) (void*) ) _handler,
			(void*) connection
			);

		CHECK_ERRNO (status, "Create handle connection thread");

		pthread_detach (client_thread);

		CHECK_ERRNO (status, "Detach connection thread");

		pthread_mutex_lock (&connections_lock);

		if (save_conn (connection) ) {
			/* Опасная ситуация */
			fprintf (stderr, "No free place for connection\n");

			TRACE;
			dump_connection_vars (connection);

			dump_connections ();

			pthread_mutex_unlock (&connections_lock);
			pthread_mutex_unlock (&init_connection_lock);
			exit (EXIT_FAILURE);
		}

		pthread_mutex_unlock (&connections_lock);
		pthread_mutex_unlock (&init_connection_lock);
	}
}

/**
 * Дескриптор сокета сервера.
 * Должен быть глобальным для корректного закрытия на выходе.
 */
int server_sockfd;

/**
 * Закрыть сокет сервера.
 *
 * @return статус завершения.
 */
int close_server_sockfd (void)
{
	int status;

	status = shutdown (server_sockfd, SHUT_RDWR);

	CHECK_ERRNO (status, "Shutdown server socket");

	status = close (server_sockfd);

	CHECK_ERRNO (status, "Close server socket");

	return status;
}

/**
 * Закрыть сокет сервера во время нормального выхода из программы.
 */
void close_server_socfd_on_exit (void)
{
	TRACE;

	printf ("\nAll connects: %lld\n", connections_count);

	pthread_mutex_unlock (&connections_lock);
	pthread_mutex_unlock (&init_connection_lock);
	fprintf (stderr, "\nWaiting for close connections...");
	int remain = 10;	/* Время для ожидания */

	do {
		pthread_mutex_lock (&connections_lock);

		if (0 == connections) {
			printf (" done\n");
			return;
		}

		pthread_mutex_unlock (&connections_lock);

		sleep (1);
		fprintf (stderr, "%d ", --remain);
	}
	while (remain > 0);

	fprintf (stderr, "\n");

	DBG_printf ("Connections at start freeing: %d\n", connections);

	/* Для корректного закрытия в середине инициализации соединения */
	pthread_mutex_lock (&init_connection_lock);
	close_server_sockfd();
	pthread_mutex_unlock (&init_connection_lock);

	pthread_mutex_lock (&connections_lock);
	free_all_conn();
	pthread_mutex_unlock (&connections_lock);

	DBG_printf ("Connections at close: %d\n", connections);

	if (0 != connections) {
		fprintf (stderr, "Not all connections were closed (%d)",
			 connections);
	}
}

/**
 * Корректный выход из процесса.
 * Используется для обработки SIGINT (Ctrl + C).
 *
 * @param[in] signum номер сигнала (только SIGINT).
 */
void gracefully_exit (IN int signum)
{
	TRACE;

	exit (EXIT_SUCCESS);
}

/**
 * Точка входа в приложение.
 *
 * @param[in] argc количество аргументов командной строки.
 * @param[in] argv массив аргументов.
 * @return статус завершения.
 */
int main (IN int argc, IN char** argv)
{
	TRACE;

	int listen_port = LISTEN_PORT;

	if (argc > 1) {
		listen_port = atoi (argv[1]);
	}

	pthread_mutex_init (&connections_lock, NULL);
	pthread_mutex_init (&init_connection_lock, NULL);

	memset (current_connections, 0, HANDLE_CONNS_COUNT);

	server_sockfd = socket (AF_INET, SOCK_STREAM,
				getprotobyname ("TCP")->p_proto);

	CHECK_ERRNO (server_sockfd, "Socket create");

	/* REUSEADDR не используется в связи с проблемами безопасности */

	struct sockaddr_in addr;
	memset (&addr, 0, sizeof (struct sockaddr) );

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl (INADDR_ANY);
	addr.sin_port = htons (listen_port);

	int status = bind (server_sockfd, (struct sockaddr*) &addr,
			   sizeof (addr) );

	CHECK_ERRNO (status, "Bind");

	atexit (close_server_socfd_on_exit);
	signal (SIGINT, gracefully_exit);

	status = listen (server_sockfd, LISTEN_BACKLOG);

	CHECK_ERRNO (status, "Start listening");

	status = connections_loop (server_sockfd, handler);

	CHECK_ERRNO (status, "Connection loop");

	status = close_server_sockfd();

	return status;
}
