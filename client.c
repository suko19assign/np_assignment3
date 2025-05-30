int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s HOST:PORT NICK\n", argv[0]);
        return EXIT_FAILURE;
    }
