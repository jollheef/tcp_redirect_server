/**
 * @file echo_test.c
 * @author Михаил Клементьев < jollheef <AT> riseup.net >
 * @date Март 2015
 * @license GPLv3
 * @brief echo test application
 *
 * Выводит в стандартный поток вывода и ошибок то, что вводит пользователь.
 */

#include <stdio.h>
#include <stdlib.h>

int main()
{
	printf("Test app\n");

	char* s;

	while (1) {
		scanf("%ms", &s);

		fprintf(stdout, "STDOUT:%s\n", s);
		fflush(stdout);

		fprintf(stderr, "STDERR:%s\n", s);
		/* Стандартный поток ошибок не буферизируется */

		free(s);
	}
}
