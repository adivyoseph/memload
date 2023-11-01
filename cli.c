#include <stdio.h>
#include <string.h>



typedef struct menu_item_s {
    char *name;
    int (*callBack)(int, char *argv[] );
    char *help;
} menu_item_t;

#define MAX_MENU_ITEMS 32
int menuItems = 0;

menu_item_t menu_items[MAX_MENU_ITEMS];

char workLine[128];
char *margv[16];

int ph_getLine(char *line);
int ph_parseLine(char *line, char*argv[]);
int ph_itemMatch(char *item, int argc, char *argv[] );
int menuAddItem(char *name, int (*)(int, char *argv[]), char *help);
int cbExit(int, char *argv[]);
int cbHelp(int, char *argv[]);

int menuInit(){
    menuAddItem("x", cbExit, "exit program");
    menuAddItem("h", cbHelp, "list help");
    return 0;
}

int menuAddItem(char *name, int (*cbFn)(int, char *argv[]) , char *help){
    menu_items[menuItems].name = name;

    menu_items[menuItems].callBack = cbFn;
    menu_items[menuItems].help = help;
    menuItems++;

    return 0;
}

int  menuLoop(){
    int margc;
        //prompt
        ph_getLine(workLine);
        margc = ph_parseLine(workLine, margv);
        return ph_itemMatch(margv[0], margc, margv);
}

int ph_itemMatch(char *item, int argc, char *argv[] ){
    int i ;

    for (i = 0; i < menuItems; i++ ) {
        if(strcmp(item, menu_items[i].name) == 0){
            return menu_items[i].callBack(argc, argv);

        }
    }
    if (i == menuItems) {
        printf("command not found\n");
    }
    return 1;


}


int ph_getLine(char *line){

    int c;
    int index = 0;

    if (line ==NULL){
        return 0;
    }

    for(;;) {
        c = fgetc(stdin);
        if(c == EOF)
            break;
        if (c =='\n') {
            if (index== 0) {
                continue;
            }
            break;
        }
        if (c == '\t') {
            c = ' ';
        }
        line[index++] = c;
        
    }

    //add string termination

//    line[index++] = ' ';
    line[index++] = '\0';
   // printf("line '%s'\n", line);
    return index;
    
}

int ph_parseLine(char *line, char*argv[]){

    char *p = line;
    int  found = 0;
    int i = 0;
    int start = 1;
    int valid = 0;


    while (*p != '\0') {
       // printf("loop s%d %x \n", start, *p);
       //remove leading spaces
        if(start == 1 ) {
            if ( *p == ' ') {
                //printf("  space\n");
                p++;
                continue;
            } else {
                start = 0;
                //printf("   restart\n");
                continue;
            }
        }
        if((*p >= 0x30 && *p <= 0x39) ||
            (*p >= 0x41 && *p <= 0x5a) ||
           (*p >= 0x61 && *p <= 0x7a)) {
            //printf("     char %x\n", *p);
            if (valid == 0) {

                //first char
                argv[found] = p;
                //printf("  %d start tocken %s\n", found, p);
            }
            valid = 1;
            p++;
            continue;
        }
        else {
            if (valid) {
                //printf("     end tocken\n");
                *p++ = '\0';
                start = 1;
                valid = 0;
                found++;
            }
            else {
                start = 1;
                p++;
            }
        }
    }

    
    for (i = 0; i < found; i++) {
 //       printf("%d %s\n", i, argv[i]);
   
    }
    
   

    return found;
}



int cbExit(int argc, char *argv[] ){
    return 0;

}

int cbHelp(int argc, char *argv[] ){
    int i;
    for (i = 0; i < menuItems; i++) {
        printf("%s\t%s\n",
               menu_items[i].name,
               menu_items[i].help);
    }
    
    return 1;
}

