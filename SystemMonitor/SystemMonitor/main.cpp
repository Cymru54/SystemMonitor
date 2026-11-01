#include <ncurses.h>
#include <sys/utsname.h>
#include <sys/sysctl.h>
#include <time.h>
#include <string.h>
#include <mach/mach_init.h>
#include <mach/mach_error.h>
#include <mach/mach_host.h>
#include <mach/vm_map.h>
#include <libproc.h>
#include <ifaddrs.h>
#include <net/if.h>

#define HEADER_HEIGHT 3

int main(int argc, const char * argv[]) {
	initscr();
	cbreak();
	noecho();
	curs_set(0);
	keypad(stdscr, TRUE);
	timeout(1000);
	
	start_color();
	init_pair(1, COLOR_RED, COLOR_BLACK);
	init_pair(2, COLOR_CYAN, COLOR_BLACK);
	init_pair(3, COLOR_WHITE, COLOR_BLACK);

	int max_y, max_x;
	getmaxyx(stdscr, max_y, max_x);
	
	int content_height = max_y - HEADER_HEIGHT;
	int half_height = content_height / 2;

	WINDOW *header_win = newwin(HEADER_HEIGHT, max_x, 0, 0);
	WINDOW *cpu_win = newwin(half_height, max_x / 2, HEADER_HEIGHT, 0);
	WINDOW *mem_win = newwin(half_height, max_x - (max_x / 2), HEADER_HEIGHT, max_x / 2);
	WINDOW *net_win = newwin(content_height - half_height, max_x, HEADER_HEIGHT + half_height, 0);
	
	host_cpu_load_info_data_t prev_cpu_load;
	memset(&prev_cpu_load, 0, sizeof(prev_cpu_load));
	
	uint64_t total_ram = 0;
	size_t len = sizeof(total_ram);
	sysctlbyname("hw.memsize", &total_ram, &len, NULL, 0);
	vm_size_t page_size;
	host_page_size(mach_host_self(), &page_size);
	
	unsigned long long prev_ibytes = 0;
	unsigned long long prev_obytes = 0;

	int ch;
	while((ch = getch()) != 'q') {
		struct utsname sys_info;
		uname(&sys_info);
		
		struct timeval boottime;
		size_t size = sizeof(boottime);
		int mib[2] = {CTL_KERN, KERN_BOOTTIME};
		sysctl(mib, 2, &boottime, &size, NULL, 0);
		
		time_t now = time(NULL);
		time_t uptime_sec = now - boottime.tv_sec;
		
		mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
		host_cpu_load_info_data_t cpu_load;
		host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&cpu_load, &count);
		
		unsigned long long user_ticks = cpu_load.cpu_ticks[CPU_STATE_USER] - prev_cpu_load.cpu_ticks[CPU_STATE_USER];
		unsigned long long sys_ticks = cpu_load.cpu_ticks[CPU_STATE_SYSTEM] - prev_cpu_load.cpu_ticks[CPU_STATE_SYSTEM];
		unsigned long long idle_ticks = cpu_load.cpu_ticks[CPU_STATE_IDLE] - prev_cpu_load.cpu_ticks[CPU_STATE_IDLE];
		unsigned long long nice_ticks = cpu_load.cpu_ticks[CPU_STATE_NICE] - prev_cpu_load.cpu_ticks[CPU_STATE_NICE];
		unsigned long long total_ticks = user_ticks + sys_ticks + idle_ticks + nice_ticks;
		
		double user_pct = 0.0, sys_pct = 0.0, idle_pct = 100.0;
		if (total_ticks > 0) {
			user_pct = (double)user_ticks / total_ticks * 100.0;
			sys_pct = (double)sys_ticks / total_ticks * 100.0;
			idle_pct = (double)idle_ticks / total_ticks * 100.0;
		}
		prev_cpu_load = cpu_load;
		
		int pids[2048];
		int num_procs = proc_listallpids(pids, sizeof(pids));
		int total_processes = 0;
		int total_threads = 0;
		
		for (int i = 0; i < num_procs; i++) {
			struct proc_taskinfo pti;
			if (proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &pti, sizeof(pti)) == sizeof(pti)) {
				total_processes++;
				total_threads += pti.pti_threadnum;
			}
		}
		
		vm_statistics64_data_t vm_stat;
		mach_msg_type_number_t count_vm = HOST_VM_INFO64_COUNT;
		host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm_stat, &count_vm);
		
		double gigabyte = 1024.0 * 1024.0 * 1024.0;
		double total_mem_gb = (double)total_ram / gigabyte;
		double wired_mem = (double)vm_stat.wire_count * page_size / gigabyte;
		double active_mem = (double)vm_stat.active_count * page_size / gigabyte;
		double compressed_mem = (double)vm_stat.compressor_page_count * page_size / gigabyte;
		double free_mem = (double)vm_stat.free_count * page_size / gigabyte;
		double used_mem = wired_mem + active_mem + compressed_mem;
		
		struct ifaddrs *ifap, *ifa;
		unsigned long long current_ibytes = 0;
		unsigned long long current_obytes = 0;
		
		if (getifaddrs(&ifap) == 0) {
			for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
				if (ifa->ifa_addr != NULL && ifa->ifa_addr->sa_family == AF_LINK) {
					if (strncmp(ifa->ifa_name, "en", 2) == 0) {
						struct if_data *ifd = (struct if_data *)ifa->ifa_data;
						current_ibytes += ifd->ifi_ibytes;
						current_obytes += ifd->ifi_obytes;
					}
				}
			}
			freeifaddrs(ifap);
		}
		
		double dl_speed = 0.0, ul_speed = 0.0;
		if (prev_ibytes > 0 && prev_obytes > 0) {
			dl_speed = (double)(current_ibytes - prev_ibytes) / 1024.0;
			ul_speed = (double)(current_obytes - prev_obytes) / 1024.0;
		}
		prev_ibytes = current_ibytes;
		prev_obytes = current_obytes;
		
		char dl_str[32], ul_str[32];
		if (dl_speed >= 1024.0) sprintf(dl_str, "%6.2f MB/s", dl_speed / 1024.0);
		else sprintf(dl_str, "%6.2f KB/s", dl_speed);
		if (ul_speed >= 1024.0) sprintf(ul_str, "%6.2f MB/s", ul_speed / 1024.0);
		else sprintf(ul_str, "%6.2f KB/s", ul_speed);
		
		int days = (int)(uptime_sec / 86400);
		int hours = (int)((uptime_sec % 86400) / 3600);
		int mins = (int)((uptime_sec % 3600) / 60);
		int secs = (int)(uptime_sec % 60);
		
		werase(header_win);
		werase(cpu_win);
		werase(mem_win);
		werase(net_win);
		
		box(header_win, 0, 0);
		box(cpu_win, 0, 0);
		box(mem_win, 0, 0);
		box(net_win, 0, 0);
		
		mvwprintw(header_win, 0, 2, " SYSTEM INFO ");
		mvwprintw(cpu_win, 0, 2, " CPU ");
		mvwprintw(mem_win, 0, 2, " MEMORY ");
		mvwprintw(net_win, 0, 2, " NETWORK ");
		mvwprintw(header_win, 1, 2, "Hostname: %s | Kernel: %s %s | Uptime: %d Days, %02d:%02d:%02d", sys_info.nodename, sys_info.sysname, sys_info.release, days, hours, mins, secs);
		
		int cpu_win_width = max_x / 2;
		int mid_x = cpu_win_width / 2;
		mvwprintw(cpu_win, 2, 2, "System:");
		wattron(cpu_win, COLOR_PAIR(1));
		mvwprintw(cpu_win, 2, 12, "%5.2f%%", sys_pct);
		wattroff(cpu_win, COLOR_PAIR(1));
		mvwprintw(cpu_win, 4, 2, "User:");
		wattron(cpu_win, COLOR_PAIR(2));
		mvwprintw(cpu_win, 4, 12, "%5.2f%%", user_pct);
		wattroff(cpu_win, COLOR_PAIR(2));
		mvwprintw(cpu_win, 6, 2, "Idle:");
		mvwprintw(cpu_win, 6, 12, "%5.2f%%", idle_pct);
		mvwprintw(cpu_win, 3, mid_x + 2, "Threads:");
		mvwprintw(cpu_win, 3, mid_x + 12, "%6d", total_threads);
		mvwprintw(cpu_win, 5, mid_x + 2, "Processes:");
		mvwprintw(cpu_win, 5, mid_x + 12, "%6d", total_processes);
		
		int mem_win_width = max_x - cpu_win_width;
		int mem_mid_x = mem_win_width / 2;
		mvwprintw(mem_win, 2, 2, "Total:");
		mvwprintw(mem_win, 2, 11, "%5.2f GB", total_mem_gb);
		mvwprintw(mem_win, 4, 2, "Used:");
		mvwprintw(mem_win, 4, 11, "%5.2f GB", used_mem);
		mvwprintw(mem_win, 6, 2, "Free:");
		mvwprintw(mem_win, 6, 11, "%5.2f GB", free_mem);
		mvwprintw(mem_win, 2, mem_mid_x + 2, "Wired:");
		mvwprintw(mem_win, 2, mem_mid_x + 10, "%5.2f GB", wired_mem);
		mvwprintw(mem_win, 4, mem_mid_x + 2, "Active:");
		mvwprintw(mem_win, 4, mem_mid_x + 10, "%5.2f GB", active_mem);
		mvwprintw(mem_win, 6, mem_mid_x + 2, "Compr:");
		mvwprintw(mem_win, 6, mem_mid_x + 10, "%5.2f GB", compressed_mem);
		
		int net_mid_x = max_x / 2;
		mvwprintw(net_win, 2, 4, "Download (RX):");
		mvwprintw(net_win, 2, 20, "%s", dl_str);
		mvwprintw(net_win, 2, net_mid_x + 4, "Upload (TX):");
		mvwprintw(net_win, 2, net_mid_x + 18, "%s", ul_str);
		
		wrefresh(header_win);
		wrefresh(cpu_win);
		wrefresh(mem_win);
		wrefresh(net_win);
	}

	delwin(header_win);
	delwin(cpu_win);
	delwin(mem_win);
	delwin(net_win);
	endwin();

	return 0;
}
