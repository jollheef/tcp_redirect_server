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
#define _BSD_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>

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
#define HANDLE_CONNS_COUNT (1000)

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
 * Сообщение указывает на достижение сервером общего лимита соединений.
 */
#define ALL_CONN_LIMIT_MSG ("Connection closed by general connections limit.\n")

/**
 * Сообщение о достижение клинта общего лимита соединений с одного ip.
 */
#define IP_CONN_LIMIT_MSG ("Connection closed by ip connections limit.\n")

/**
 * Сообщение о истечении времени ожидания ответа.
 */
#define CONN_TIMEOUT_MSG ("Connection closed by timeout.\n")

/**
 * Время ожидания ответа пользователя, в миллисекундах.
 */
#define TIMEOUT_MS (3000)	/* 3 секунды */

/**
 * Максимальный размер посылки, в байтах.
 * Сделано для исключения ситуации посылки серверу данных больше,
 * чем у сервера памяти, а также от генерации приложением слишком
 * большой посылки, которая может загрузить сеть.
 */
#define MAX_DATA_COUNT (1024 * 1024)

/**
 * Максимальное количество соединений с одного IP
 */
#define MAX_IP_CONN (10)

/**
 * Максимальное количество IP в массиве счетчиков соединений.
 * Изменение этого значения может привести к непредсказуемому результату.
 */
#define IP_CONNECTIONS_MAX (255*255)

/**
 * Массив счетчиков соединений для каждого IP.
 * Используется только 2 последних октета IP.
 */
ushort ip_connections[IP_CONNECTIONS_MAX];

/**
 * Получить индекс для массива IP соединений.
 * Индекс это произведение двух последних октетов.
 *
 * @param[in] s_addr интернет адрес.
 * @return индекс 0..65535 (IP_CONNECTIONS_MAX).
 */
static inline ushort get_ip_index(IN in_addr_t s_addr)
{
	return ntohl(s_addr) & 65535;
}

/**
 * Получить количество текущих соединений для IP.
 * Программа рассчитана на работу в /16, в бОльших подсетях поведение
 * блокировки количества соединений по IP будет отличным от ожидаемого.
 * Собственно, для возможной поддержки бОльших подсетей и вынесены
 * операции работы с количеством соединений в отдельные функции.
 *
 * @param[in] s_addr интернет адрес.
 * @return количество соединений для ip.
 */
static inline ushort get_ip_conn(IN in_addr_t s_addr)
{
	return ip_connections[get_ip_index(s_addr)];
}

/**
 * Увеличить счетчик соединений с IP на единицу.
 *
 * @param[in] s_addr интернет адрес.
 */
static inline void inc_ip_conn(IN in_addr_t s_addr)
{
	++ip_connections[get_ip_index(s_addr)];
}

/**
 * Уменьшить счетчик соединений с IP на единицу.
 *
 * @param[in] s_addr интернет адрес.
 */
static inline void dec_ip_conn(IN in_addr_t s_addr)
{
	--ip_connections[get_ip_index(s_addr)];
}

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
void dump_connection_vars(IN struct connection_vars_t* conn)
{
	printf("Адрес структуры: %p\t|", conn);
	printf("Номер соединения: %lld\t|", conn->n);
	printf("Дескриптор сокета клиента: %d\t|", conn->sockfd);
	printf("Описание соединения клиента: %s:%d\n",
	       inet_ntoa(conn->addr.sin_addr), ntohs(conn->addr.sin_port));
}

/**
 * Массив открытых на данный момент соединений.
 */
struct connection_vars_t* current_connections[HANDLE_CONNS_COUNT];

/**
 * Вывод состояния сохраненных соединений в стандартный вывод.
 */
void dump_connections(void)
{
	for (int i = 0; i < HANDLE_CONNS_COUNT; ++i) {
		if (NULL != current_connections[i]) {
			TRACE;
			dump_connection_vars(current_connections[i]);
		}
	}
}

/**
 * Сохранить соединение в массиве соединений.
 *
 * @param[in] conn -- указатель на структуру соединения.
 */
int save_conn(IN struct connection_vars_t* conn)
{
	int status = 1;

	for (int i = 0; i < HANDLE_CONNS_COUNT; ++i) {
		if (NULL == current_connections[i]) {
			TRACE;
#ifdef DEBUG
			dump_connection_vars(conn);
#endif
			current_connections[i] = conn;

			connections += 1;

			inc_ip_conn(conn->addr.sin_addr.s_addr);

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
void remove_conn(IN struct connection_vars_t* conn)
{
	TRACE;

	for (int i = 0; i < HANDLE_CONNS_COUNT; ++i) {
		if (NULL != current_connections[i]) {
			TRACE;

			if (conn->n == current_connections[i]->n) {
				TRACE;
#ifdef DEBUG
				dump_connection_vars(conn);
#endif
				current_connections[i] = NULL;
				free(conn);
				connections -= 1;

				dec_ip_conn(conn->addr.sin_addr.s_addr);

				return;
			}
		}
	}

	fprintf(stderr, "Connection not found\n");
}

/**
 * Получить количество свободных мест в массиве соединений.
 */
int get_free_places(void)
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
void free_all_conn(void)
{
	for (int i = 0; i < HANDLE_CONNS_COUNT; ++i) {
		if (NULL == current_connections[i]) {
			continue;
		}

		struct connection_vars_t* connection = current_connections[i];

		TRACE;

#ifdef DEBUG
		dump_connection_vars(connection);

#endif
		int status = shutdown(connection->sockfd, SHUT_RDWR);

		CHECK_ERRNO(status, "Shutdown connection");

		status = close(connection->sockfd);

		CHECK_ERRNO(status, "Close connection");

		connections -= 1;

		free(current_connections[i]);

		current_connections[i] = NULL;
	}
}

/**
 * Реализация двунаправленного popen.
 *
 * @param[in] command команда для запуска.
 * @param[out] infp stdin процесса.
 * @param[out] outfp stdout и stderr процесса.
 * @return идентификатор запущенного процесса или -1 в случае ошибки (errno).
 */
pid_t popen2(IN const char* command, OUT int* infp, OUT int* outfp)
{
	int p_stdin[2], p_stdout[2];

	if (pipe(p_stdin) != 0 || pipe(p_stdout) != 0) {
		return -1;
	}

	pid_t pid = fork();

	if (pid == 0) {
		close(p_stdin[1]);

		dup2(p_stdin[0], fileno(stdin));

		close(p_stdout[0]);

		dup2(p_stdout[1], fileno(stdout));
		dup2(p_stdout[1], fileno(stderr));

		execl("/bin/sh", "sh", "-c", command, NULL);

		/* До этой строки исполнение никогда не должно дойти */
		exit(EXIT_FAILURE);
	} else if (pid < 0) {
		return -1;
	}

	*infp = p_stdin[1];

	*outfp = p_stdout[0];

	return pid;
}

/**
 * Команда, вызываемая на каждое соединения.
 */
const char* user_command = "./test";

/**
 * Взаимодействие с пользователем.
 *
 * @param[in] connection описание соединения.
 * @return статус завершения.
 */
int user_interaction(IN struct connection_vars_t* conn)
{
	TRACE;

	int infp, outfp;
	int pid = popen2(user_command, &infp, &outfp);

	CHECK_ERRNO(pid, "Execute application");

	char* buf = calloc(1, sizeof(char));

	struct pollfd* fds = calloc(sizeof(struct pollfd), 2);
	fds[0].fd = outfp;
	fds[1].fd = conn->sockfd;
	fds[0].events = fds[1].events = POLLIN;

	while (true) {
		int status = poll(fds, 2, TIMEOUT_MS);

		CHECK_ERRNO(status, "Polling");

		DBG_printf("Revents: %d %d\n", fds[0].revents, fds[1].revents);

		if ((fds[0].revents | fds[1].revents) & POLLNVAL) {
			/* Один из дескрипторов закрылся */
			DBG_printf("One of fd has been closed\n");
			break;
		}

		if (0 == status) {
			/* Время ожидания истекло */
			send(conn->sockfd, CONN_TIMEOUT_MSG,
			     sizeof(CONN_TIMEOUT_MSG), 0);
			break;
		}

		int count = 0;	/* Количество данных в fd */

		status = ioctl(conn->sockfd, FIONREAD, &count);

		CHECK_ERRNO(status, "Get available data in sockfd");

		DBG_printf("%d bytes to read from socket\n", count);

		if ((0 == count) && (fds[1].revents & POLLIN)) {
			/*
			 * Если poll возвращает, что данные есть,
			 * но эти данные нельзя прочитать, то
			 * скорее всего сокет закрылся, а эти данные
			 * это что-то для сетевого стека, а не для нас.
			 */
			DBG_printf("Socket most likely closed\n");
			break;
		}

		if (count > MAX_DATA_COUNT) {
			/* TODO: Возможно, стоит генерировать сообщение. */
			break;
		}

		if (count > 0) {
			TRACE;

			buf = realloc(buf, count);

			int ret = recv(conn->sockfd, buf, count, 0);

			DBG_printf("%d bytes ret from socket\n", ret);

			if (ret < 0) {
				break;
			}

			ret = write(infp, buf, ret);

			if (ret < 0) {
				break;
			}
		}

		status = ioctl(outfp, FIONREAD, &count);

		CHECK_ERRNO(status, "Get available data in outfp");

		DBG_printf("%d bytes to read from outfp\n", count);

		if (count > MAX_DATA_COUNT) {
			/* TODO: Возможно, стоит генерировать сообщение. */
			break;
		}

		if (count > 0) {
			TRACE;

			buf = realloc(buf, count);

			int ret = read(outfp, buf, sizeof(buf));

			DBG_printf("%d bytes ret from outfp\n", ret);

			if (ret < 0) {
				break;
			}

			ret = send(conn->sockfd, buf, ret, 0);

			if (ret < 0) {
				break;
			}
		}
	}

	close(infp);
	close(outfp);

	free(buf);
	free(fds);

	kill(pid, SIGTERM);

	TRACE;

	return 0;
}

/**
 * Обработчик соединения.
 *
 * @param[in] connection описание соединения.
 * @return не используется.
 */
void* handler(IN struct connection_vars_t* connection)
{
	/* Поток должен запуститься только после окончания инициализации */
	pthread_mutex_lock(&init_connection_lock);
	pthread_mutex_unlock(&init_connection_lock);

	TRACE;

	int status = user_interaction(connection);

	CHECK_ERRNO(status, "User interaction");

	TRACE;

#ifdef DEBUG
	dump_connection_vars(connection);
#endif

	status = shutdown(connection->sockfd, SHUT_RDWR);

	CHECK_ERRNO(status, "Shutdown connection");

	TRACE;

	status = close(connection->sockfd);

	CHECK_ERRNO(status, "Close connection");

	pthread_mutex_lock(&connections_lock);
	remove_conn(connection);
	pthread_mutex_unlock(&connections_lock);

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
connections_loop(IN int server_sockfd,
                 IN void * (*_handler)(struct connection_vars_t*))
{
	int status = 0;

	while (true) {
		TRACE;

#ifdef DEBUG
		pthread_mutex_lock(&connections_lock);
		printf("Connections: %d\n", connections);
		printf("Free places: %d\n", get_free_places());

		if (HANDLE_CONNS_COUNT != (connections + get_free_places())) {
			fprintf(stderr, "Something went wrong\n");

			TRACE;
			dump_connections();

			pthread_mutex_unlock(&connections_lock);
			return -EINVAL;
		}

		pthread_mutex_unlock(&connections_lock);
#endif

		struct sockaddr_in client_addr;

		memset(&client_addr, 0, sizeof(struct sockaddr_in));

		socklen_t client_addr_size = sizeof(struct sockaddr_in);

		int client_sockfd = accept(
			server_sockfd,
			(struct sockaddr*) &client_addr,
			&client_addr_size
                        );

		CHECK_ERRNO(client_sockfd, "Accept connection");

		/* Проверка лимита соединений для IP */
		pthread_mutex_lock(&connections_lock);
		int ip_conn = get_ip_conn(client_addr.sin_addr.s_addr);
		pthread_mutex_unlock(&connections_lock);

		if (ip_conn > MAX_IP_CONN) {
			TRACE;

			/* Отправка клиенту сообщение о лимите */
			send(client_sockfd, IP_CONN_LIMIT_MSG,
			     sizeof(IP_CONN_LIMIT_MSG), 0);

			shutdown(client_sockfd, SHUT_RDWR);
			close(client_sockfd);

			continue;
		}

		/* Проверка общего лимита соединений. */
		pthread_mutex_lock(&connections_lock);
		int is_limit = connections > HANDLE_CONNS_COUNT - 1;
		pthread_mutex_unlock(&connections_lock);

		if (is_limit) {
			TRACE;

			/* Отправка клиенту сообщение о лимите */
			send(client_sockfd, ALL_CONN_LIMIT_MSG,
			     sizeof(ALL_CONN_LIMIT_MSG), 0);

			shutdown(client_sockfd, SHUT_RDWR);
			close(client_sockfd);

			continue;
		}

		pthread_mutex_lock(&init_connection_lock);

		++connections_count;

		DBG_printf("Connect: %lld\n", connections_count);

		pthread_t client_thread;

		struct connection_vars_t* connection =
			calloc(sizeof(struct connection_vars_t), 1);

		if (NULL == connection) {
			TRACE;
			fprintf(stderr, "Allocation failed\n");
			exit(EXIT_FAILURE);
		}

		TRACE;

		connection->sockfd = client_sockfd;
		connection->addr = client_addr;
		connection->n = connections_count;

		TRACE;

		status = pthread_create(
			&client_thread,
			NULL,
			(void * (*)(void*)) _handler,
			(void*) connection
			);

		CHECK_ERRNO(status, "Create handle connection thread");

		pthread_detach(client_thread);

		CHECK_ERRNO(status, "Detach connection thread");

		pthread_mutex_lock(&connections_lock);

		if (save_conn(connection)) {
			/* Опасная ситуация */
			fprintf(stderr, "No free place for connection\n");

			TRACE;
			dump_connection_vars(connection);

			dump_connections();

			pthread_mutex_unlock(&connections_lock);
			pthread_mutex_unlock(&init_connection_lock);
			exit(EXIT_FAILURE);
		}

		pthread_mutex_unlock(&connections_lock);
		pthread_mutex_unlock(&init_connection_lock);
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
int close_server_sockfd(void)
{
	int status;

	status = shutdown(server_sockfd, SHUT_RDWR);

	CHECK_ERRNO(status, "Shutdown server socket");

	status = close(server_sockfd);

	CHECK_ERRNO(status, "Close server socket");

	return status;
}

/**
 * Закрыть сокет сервера во время нормального выхода из программы.
 */
void close_server_socfd_on_exit(void)
{
	TRACE;

	printf("\nAll connects: %lld\n", connections_count);

	pthread_mutex_unlock(&connections_lock);
	pthread_mutex_unlock(&init_connection_lock);
	fprintf(stderr, "\nWaiting for close connections...");
	int remain = 10;	/* Время для ожидания */

	do {
		pthread_mutex_lock(&connections_lock);

		if (0 == connections) {
			printf(" done\n");
			return;
		}

		pthread_mutex_unlock(&connections_lock);

		sleep(1);
		fprintf(stderr, "%d ", --remain);
	}
	while (remain > 0);

	fprintf(stderr, "\n");

	DBG_printf("Connections at start freeing: %d\n", connections);

	/* Для корректного закрытия в середине инициализации соединения */
	pthread_mutex_lock(&init_connection_lock);
	close_server_sockfd();
	pthread_mutex_unlock(&init_connection_lock);

	pthread_mutex_lock(&connections_lock);
	free_all_conn();
	pthread_mutex_unlock(&connections_lock);

	DBG_printf("Connections at close: %d\n", connections);

	if (0 != connections) {
		fprintf(stderr, "Not all connections were closed (%d)",
			connections);
	}
}

/**
 * Корректный выход из процесса.
 * Используется для обработки SIGINT (Ctrl + C).
 *
 * @param[in] signum номер сигнала (только SIGINT).
 */
void gracefully_exit(IN int signum)
{
	TRACE;

	exit(EXIT_SUCCESS);
}

/**
 * Точка входа в приложение.
 *
 * @param[in] argc количество аргументов командной строки.
 * @param[in] argv массив аргументов.
 * @return статус завершения.
 */
int main(IN int argc, IN char** argv)
{
	TRACE;

	int listen_port = LISTEN_PORT;

	if (argc > 1) {
		user_command = argv[1];
	}

	if (argc > 2) {
		listen_port = atoi(argv[2]);
	}

	printf("Usage: %s command port\n\n", argv[0]);

	printf("Command: %s.\n", user_command);
	printf("Warning: use fflush(stdout) for output.\n");
	printf("Port: %d.\n", listen_port);

	pthread_mutex_init(&connections_lock, NULL);
	pthread_mutex_init(&init_connection_lock, NULL);

	memset(current_connections, 0, HANDLE_CONNS_COUNT);

	memset(ip_connections, 0, sizeof(ip_connections));

	server_sockfd = socket(AF_INET, SOCK_STREAM,
			       getprotobyname("TCP")->p_proto);

	CHECK_ERRNO(server_sockfd, "Socket create");

	/* REUSEADDR не используется в связи с проблемами безопасности */

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(struct sockaddr));

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(listen_port);

	int status = bind(server_sockfd, (struct sockaddr*) &addr,
			  sizeof(addr));

	CHECK_ERRNO(status, "Bind");

	atexit(close_server_socfd_on_exit);
	signal(SIGINT, gracefully_exit);

	status = listen(server_sockfd, LISTEN_BACKLOG);

	CHECK_ERRNO(status, "Start listening");

	status = connections_loop(server_sockfd, handler);

	CHECK_ERRNO(status, "Connection loop");

	status = close_server_sockfd();

	return status;
}
