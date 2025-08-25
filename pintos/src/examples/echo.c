// #include <stdio.h>
// #include <string.h>
// #include <syscall.h>
// int main(int argc, char **argv) {
//   int i;
//   if (argc == 1) {
//     printf("\n");
//     return EXIT_SUCCESS;
//   }

//   for (i = 1; i < argc; i++) 
//   {
//     int len = strlen(argv[i]);
//     if (argv[i][0] == '\'') {
//       argv[i][len - 1] = '\0';
//     }
//     argv[i]++;
//     printf("%s ", argv[i]);
//   }
//   printf("\n");

//   return EXIT_SUCCESS;
// }


#include <stdio.h>
#include <syscall.h>
#include <string.h>

int main (int argc, char **argv)
{
  // printf("This is me changing the echo command yay\n");
  int i;

  if(argc == 1)
  {
    return EXIT_SUCCESS;
  }


  for (i = 1; i < argc; i++)
    {
      if(argv[i][0] == '\'')
      {
        //removal of the quotes
        argv[i][strlen(argv[i]) - 1] = '\0';
        argv[i]++; //move it one spot front so that we dont have to print that quote
      }
      printf ("%s ", argv[i]);
    }

  printf ("\n");

  return EXIT_SUCCESS;
}