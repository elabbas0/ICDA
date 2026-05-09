#ifndef SERIAL_H
#define SERIAL_H

void serial_init(void);
void serial_write_char(char c);
void serial_write(const char *str);
int  serial_ready(void);

#endif
