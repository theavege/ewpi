
int ewpi_spawn(const char *host, const char *prog, const char *option);

/* Same as above, but passing NULL for prog/option means "no argument"
 * instead of the literal empty string. */
int ewpi_spawnv(const char *host, const char *prog, const char *arg);
