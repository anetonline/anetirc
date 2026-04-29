/* bridge.h - DOS-side API for the file-based door/helper bridge. */
#ifndef ANET_DOS_BRIDGE_H
#define ANET_DOS_BRIDGE_H
void bridge_set_node(int n);
int  bridge_connect_local(void);
void bridge_close(void);
int  bridge_send_line(const char *line);
int  bridge_poll_line(char *out, int outsz);
#endif
