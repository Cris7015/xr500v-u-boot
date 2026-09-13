/* SPDX-License-Identifier: GPL-2.0-or-later */
extern void start_spram(void);

void init_arch(int argc, char **argv, char **envp, int *prom_vec)
{
	(void)argc;
	(void)argv;
	(void)envp;
	(void)prom_vec;
	start_spram();
}

void setup_arch(void)
{
}
