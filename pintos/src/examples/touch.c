#include <stdio.h>
#include <syscall.h>
// #include <stdlib.h>

int main(int argc, char *argv[]) 
{
    for(int i = 1; i<argc; i++)
    {
        int flag = create(argv[i], 0);
        if(!flag)
        {
            printf("%s: create failed\n", argv[i]);
        }
        else
        {
            printf("%s: created\n", argv[i]);
        }
    }
    return EXIT_SUCCESS;
}