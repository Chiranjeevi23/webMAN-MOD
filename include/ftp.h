#define FTP_RECV_SIZE  1024

static void absPath(char* absPath_s, const char* path, const char* cwd)
{
	if(path[0] == '/') strcpy(absPath_s, path);
	else
	{
		strcpy(absPath_s, cwd);

		if(cwd[strlen(cwd) - 1] != '/') strcat(absPath_s, "/");

		strcat(absPath_s, path);
	}

	if(islike(absPath_s, "/dev_blind") && !isDir("/dev_blind")) enable_dev_blind(NO_MSG);
}

static int ssplit(const char* str, char* left, int lmaxlen, char* right, int rmaxlen)
{
	int ios = strcspn(str, " ");
	int ret = (ios < (int)strlen(str) - 1);
	int lsize = MIN(ios, lmaxlen);

	strncpy(left, str, lsize);
	left[lsize] = '\0';

	if(ret)
	{
		strncpy(right, str + ios + 1, rmaxlen);
		right[rmaxlen] = '\0';
	}
	else
	{
		right[0] = '\0';
	}

	return ret;
}

static void handleclient_ftp(u64 conn_s_ftp_p)
{
	int conn_s_ftp = (int)conn_s_ftp_p; // main communications socket
	int data_s = -1;			// data socket
	int data_ls = -1;

	int connactive = 1;			// whether the ftp connection is active or not
	int dataactive = 0;			// prevent the data connection from being closed at the end of the loop
	u8 loggedin = 0;			// whether the user is logged in or not

	char cwd[MAX_PATH_LEN];	// Current Working Directory
	int rest = 0;			// for resuming file transfers

	char cmd[16], param[MAX_PATH_LEN], filename[MAX_PATH_LEN], source[MAX_PATH_LEN]; // used as source parameter in RNFR and COPY commands
	char buffer[FTP_RECV_SIZE], *cpursx = filename, *tempcwd = filename, *d_path = param;
	struct CellFsStat buf;
	int fd;

	int p1x = 0;
	int p2x = 0;

	#define FTP_OK_150			"150 OK\r\n"						// File status okay; about to open data connection.
	#define FTP_OK_200			"200 OK\r\n"						// The requested action has been successfully completed.
	#define FTP_OK_TYPE_200		"200 TYPE OK\r\n"					// The requested action has been successfully completed.
	#define FTP_OK_TYPE_220		"220-VSH ftpd\r\n"					// Service ready for new user.
	#define FTP_OK_221			"221 BYE\r\n"						// Service closing control connection.
	#define FTP_OK_226			"226 OK\r\n"						// Closing data connection. Requested file action successful (for example, file transfer or file abort).
	#define FTP_OK_ABOR_226		"226 ABOR OK\r\n"					// Closing data connection. Requested file action successful
	#define FTP_OK_230			"230 OK\r\n"						// User logged in, proceed. Logged out if appropriate.
	#define FTP_OK_USER_230		"230 Already in\r\n"				// User logged in, proceed.
	#define FTP_OK_250			"250 OK\r\n"						// Requested file action okay, completed.
	#define FTP_OK_331			"331 OK\r\n"						// User name okay, need password.
	#define FTP_OK_REST_350		"350 REST command successful\r\n"	// Requested file action pending further information
	#define FTP_OK_RNFR_350		"350 RNFR OK\r\n"					// Requested file action pending further information

	#define FTP_ERROR_425		"425 Error\r\n"						// Can't open data connection.
	#define FTP_ERROR_430		"430 Error\r\n"						// Invalid username or password
	#define FTP_ERROR_451		"451 Error\r\n"						// Requested action aborted. Local error in processing.
	#define FTP_ERROR_500		"500 Error\r\n"						// Syntax error, command unrecognized and the requested	action did not take place.
	#define FTP_ERROR_501		"501 Error\r\n"						// Syntax error in parameters or arguments.
	#define FTP_ERROR_REST_501	"501 No restart point\r\n"			// Syntax error in parameters or arguments.
	#define FTP_ERROR_502		"502 Not implemented\r\n"			// Command not implemented.
	#define FTP_ERROR_530		"530 Error\r\n"						// Not logged in.
	#define FTP_ERROR_550		"550 Error\r\n"						// Requested action not taken. File unavailable (e.g., file not found, no access).
	#define FTP_ERROR_RNFR_550	"550 RNFR Error\r\n"				// Requested action not taken. File unavailable


	CellRtcDateTime rDate;
	CellRtcTick pTick;

	sys_net_sockinfo_t conn_info;
	sys_net_get_sockinfo(conn_s_ftp, &conn_info, 1);

	char ip_address[16];
	char pasv_output[56];
	sprintf(ip_address, "%s", inet_ntoa(conn_info.remote_adr));

	ssend(conn_s_ftp, FTP_OK_TYPE_220); // Service ready for new user.

	if(webman_config->bind && ((conn_info.local_adr.s_addr != conn_info.remote_adr.s_addr) && strncmp(ip_address, webman_config->allow_ip, strlen(webman_config->allow_ip)) != 0))
	{
		sprintf(buffer, "451 Access Denied. Use SETUP to allow remote connections.\r\n"); ssend(conn_s_ftp, buffer);
		sclose(&conn_s_ftp);
		sys_ppu_thread_exit(0);
	}

	sprintf(ip_address, "%s", inet_ntoa(conn_info.local_adr));
	for(u8 n = 0; ip_address[n] != NULL; n++) if(ip_address[n] == '.') ip_address[n] = ',';

	sprintf(buffer, "%i webMAN ftpd " WM_VERSION "\r\n", 220); ssend(conn_s_ftp, buffer);

	strcpy(cwd, "/");

	while((connactive == 1) && working)
	{

		if(working && (recv(conn_s_ftp, buffer, FTP_RECV_SIZE, 0) > 0))
		{
			buffer[strcspn(buffer, "\n")] = '\0';
			buffer[strcspn(buffer, "\r")] = '\0';

			int split = ssplit(buffer, cmd, 15, param, MAX_PATH_LEN-1); to_upper(cmd);

			if(working && loggedin == 1)
			{
				if(IS(cmd, "CWD"))
				{
					if(split == 1)
					{
						absPath(tempcwd, param, cwd);
					}
					else
						strcpy(tempcwd, cwd);

					if(isDir(tempcwd))
					{
						strcpy(cwd, tempcwd);
						ssend(conn_s_ftp, FTP_OK_250); // Requested file action okay, completed.
					}
					else
					{
						ssend(conn_s_ftp, FTP_ERROR_550); // Requested action not taken. File unavailable (e.g., file not found, no access).
					}
				}
				else
				if(IS(cmd, "CDUP"))
				{
					int pos = strlen(cwd) - 2;

					for(int i = pos; i > 0; i--)
					{
						if(i < pos && cwd[i] == '/')
						{
							break;
						}
						else
						{
							cwd[i] = '\0';
						}
					}
					ssend(conn_s_ftp, FTP_OK_250); // Requested file action okay, completed.
				}
				else
				if(IS(cmd, "PWD"))
				{
					sprintf(buffer, "257 \"%s\"\r\n", cwd);
					ssend(conn_s_ftp, buffer);
				}
				else
				if(IS(cmd, "TYPE"))
				{
					ssend(conn_s_ftp, FTP_OK_TYPE_200); // The requested action has been successfully completed.
					dataactive = 1;
				}
				else
				if(IS(cmd, "REST"))
				{
					if(split == 1)
					{
						ssend(conn_s_ftp, FTP_OK_REST_350); // Requested file action pending further information
						rest = val(param);
						dataactive = 1;
					}
					else
					{
						ssend(conn_s_ftp, FTP_ERROR_REST_501); // Syntax error in parameters or arguments.
					}
				}
				else
				if(IS(cmd, "QUIT") || IS(cmd, "BYE"))
				{
					ssend(conn_s_ftp, FTP_OK_221);
					connactive = 0;
				}
				else
				if(IS(cmd, "FEAT"))
				{
					ssend(conn_s_ftp,	"211-Ext:\r\n"
										" SIZE\r\n"
										" MDTM\r\n"
										" PORT\r\n"
										" CDUP\r\n"
										" ABOR\r\n"
										" REST STREAM\r\n"
										" PASV\r\n"
										" LIST\r\n"
										" MLSD\r\n"
										" MLST type*;size*;modify*;UNIX.mode*;UNIX.uid*;UNIX.gid*;\r\n"
										"211 End\r\n");
				}
				else
				if(IS(cmd, "PORT"))
				{
					rest = 0;

					if(split == 1)
					{
						char data[6][4];
						u8 i = 0, k = 0, plen = strlen(param);

						for(u8 j = 0; j <= plen; j++)
						{
							if(param[j] != ',' && param[j] != 0) data[i][k++] = param[j];
							else {data[i++][k] = 0, k = 0;}
							if(i >= 6) break;
						}

						if(i == 6)
						{
							char ipaddr[16];
							sprintf(ipaddr, "%s.%s.%s.%s", data[0], data[1], data[2], data[3]);

							data_s = connect_to_server(ipaddr, getPort(val(data[4]), val(data[5])));

							if(data_s >= 0)
							{
								ssend(conn_s_ftp, FTP_OK_200);		// The requested action has been successfully completed.
								dataactive = 1;
							}
							else
							{
								ssend(conn_s_ftp, FTP_ERROR_451);	// Requested action aborted. Local error in processing.
							}
						}
						else
						{
							ssend(conn_s_ftp, FTP_ERROR_501);		// Syntax error in parameters or arguments.
						}
					}
					else
					{
						ssend(conn_s_ftp, FTP_ERROR_501);			// Syntax error in parameters or arguments.
					}
				}
				else
				if(IS(cmd, "SITE"))
				{
					if(split == 1)
					{
						split = ssplit(param, cmd, 10, filename, MAX_PATH_LEN-1); to_upper(cmd);

						if(IS(cmd, "HELP"))
						{
							ssend(conn_s_ftp, "214-CMDs:\r\n"
#ifndef LITE_EDITION
											  " SITE FLASH\r\n"
 #ifdef PKG_HANDLER
											  " SITE INSTALL <file>\r\n"
 #endif
 #ifdef EXT_GDATA
											  " SITE EXTGD <ON/OFF>\r\n"
 #endif
											  " SITE MAPTO <path>\r\n"
 #ifdef FIX_GAME
											  " SITE FIX <path>\r\n"
 #endif
											  " SITE UMOUNT\r\n"
											  " SITE COPY <file>\r\n"
											  " SITE PASTE <file>\r\n"
											  " SITE CHMOD 777 <file>\r\n"
#endif
											  " SITE SHUTDOWN\r\n"
											  " SITE RESTART\r\n"
											  "214 End\r\n");
						}
						else
						if(IS(cmd, "SHUTDOWN"))
						{
							ssend(conn_s_ftp, FTP_OK_221); // Service closing control connection.

							working = 0;
							{ DELETE_TURNOFF } { BEEP1 }
							{system_call_4(SC_SYS_POWER, SYS_SHUTDOWN, 0, 0, 0);}
							sys_ppu_thread_exit(0);
						}
						else
						if(IS(cmd, "RESTART") || IS(cmd, "REBOOT"))
						{
							ssend(conn_s_ftp, FTP_OK_221); // Service closing control connection.

							working = 0;
							{ DELETE_TURNOFF } { BEEP2 }
							if(IS(cmd, "REBOOT")) savefile(WMNOSCAN, NULL, 0);
							{system_call_3(SC_SYS_POWER, SYS_REBOOT, NULL, 0);}
							sys_ppu_thread_exit(0);
						}
						else
						if(IS(cmd, "FLASH"))
						{
							ssend(conn_s_ftp, FTP_OK_250); // Requested file action okay, completed.

							bool rw_flash = isDir("/dev_blind"); char *status = to_upper(filename);

							if(status[0] == 0) ; else
							if(IS(status, "ON" )) {if( rw_flash) continue;} else
							if(IS(status, "OFF")) {if(!rw_flash) continue;}

							if(rw_flash)
								disable_dev_blind();
							else
								enable_dev_blind(NO_MSG);
						}
#ifndef LITE_EDITION
 #ifdef PKG_HANDLER
						else
						if(IS(cmd, "INSTALL"))
						{
							absPath(param, filename, cwd); char *msg = filename;

							if(installPKG(param, msg) == 0)
								ssend(conn_s_ftp, FTP_OK_250); // Requested file action okay, completed.
							else
								ssend(conn_s_ftp, FTP_ERROR_451); // Requested action aborted. Local error in processing.

							show_msg(msg);
						}
 #endif
 #ifdef EXT_GDATA
						else
						if(IS(cmd, "EXTGD"))
						{
							ssend(conn_s_ftp, FTP_OK_250); // Requested file action okay, completed.

							char *status = to_upper(filename);

							if(status[0] == 0)		set_gamedata_status(extgd^1, true); else
							if(IS(status, "ON" ))	set_gamedata_status(0, true);		else
							if(IS(status, "OFF"))	set_gamedata_status(1, true);

						}
 #endif
						else
						if(IS(cmd, "UMOUNT"))
						{
							ssend(conn_s_ftp, FTP_OK_250); // Requested file action okay, completed.
							do_umount(true);
						}
 #ifdef COBRA_ONLY
						else
						if(IS(cmd, "MAPTO"))
						{
							ssend(conn_s_ftp, FTP_OK_250); // Requested file action okay, completed.

							char *src_path = filename;

							if(isDir(src_path))
							{
								// map current directory to path
								sys_map_path(src_path, (IS(cwd, "/") ? NULL : cwd) ); // unmap if cwd is the root
							}
							else
							{
								mount_with_mm(cwd, 1);
							}
						}
 #endif //#ifdef COBRA_ONLY
 #ifdef FIX_GAME
						else
						if(IS(cmd, "FIX"))
						{
							if(fix_in_progress)
							{
								ssend(conn_s_ftp, FTP_ERROR_451);	// Requested action aborted. Local error in processing.
							}
							else
							{
								ssend(conn_s_ftp, FTP_OK_250);		// Requested file action okay, completed.
								absPath(param, filename, cwd);

								fix_in_progress = true, fix_aborted = false;

  #ifdef COBRA_ONLY
								if(strcasestr(filename, ".iso"))
									fix_iso(param, 0x100000UL, false);
								else
  #endif //#ifdef COBRA_ONLY
									fix_game(param, filename, FIX_GAME_FORCED);

								fix_in_progress = false;
							}
						}
 #endif //#ifdef FIX_GAME
						else
						if(IS(cmd, "CHMOD"))
						{
							split = ssplit(param, cmd, 10, filename, MAX_PATH_LEN-1);

							strcpy(param, filename); absPath(filename, param, cwd);

							ssend(conn_s_ftp, FTP_OK_250); // Requested file action okay, completed.
							int attributes = val(cmd);
							if(attributes == 0)
								cellFsChmod(filename, MODE);
							else
								cellFsChmod(filename, attributes);
						}
 #ifdef COPY_PS3
						else
						if(IS(cmd, "COPY"))
						{
							sprintf(buffer, "%s %s", STR_COPYING, filename);
							show_msg(buffer);

							absPath(source, filename, cwd);
							ssend(conn_s_ftp, FTP_OK_200); // The requested action has been successfully completed.
						}
						else
						if(IS(cmd, "PASTE"))
						{
							absPath(param, filename, cwd);
							if((!copy_in_progress) && (source[0] != NULL) && (!IS(source, param)) && file_exists(source))
							{
								copy_in_progress = true; copied_count = 0;
								ssend(conn_s_ftp, FTP_OK_250); // Requested file action okay, completed.

								sprintf(buffer, "%s %s\n%s %s", STR_COPYING, source, STR_CPYDEST, param);
								show_msg(buffer);

								if(isDir(source))
									folder_copy(source, param);
								else
									file_copy(source, param, COPY_WHOLE_FILE);

								show_msg((char*)STR_CPYFINISH);
								//memset(source, 0, 512);
								copy_in_progress = false;
							}
							else
							{
								ssend(conn_s_ftp, FTP_ERROR_500);
							}
						}
 #endif
 #ifdef WM_REQUEST
						else
						if(param[0] == '/')
						{
							u16 size = sprintf(buffer, "GET %s", param);
							savefile(WMREQUEST_FILE, buffer, size);
							ssend(conn_s_ftp, FTP_OK_200); // The requested action has been successfully completed.
						}
 #endif
#endif //#ifndef LITE_EDITION
						else
						{
							ssend(conn_s_ftp, FTP_ERROR_500);
						}
					}
					else
					{
						ssend(conn_s_ftp, FTP_ERROR_501); // Syntax error in parameters or arguments.
					}
				}
				else
				if(IS(cmd, "NOOP"))
				{
					ssend(conn_s_ftp, "200 NOOP\r\n");
				}
				else
				if(IS(cmd, "MLSD") || IS(cmd, "LIST") || IS(cmd, "MLST"))
				{
					if(data_s >= 0)
					{
						bool is_MLSD = IS(cmd, "MLSD");

						int nolist = (is_MLSD || IS(cmd, "MLST"));

						if(split == 1)
						{
							strcpy(tempcwd, param);
							absPath(d_path, tempcwd, cwd);
						}
						else
							strcpy(d_path, cwd);

#if NTFS_EXT
						ntfs_md *mounts;
						int mountCount;

						mountCount = ntfsMountAll(&mounts, NTFS_DEFAULT | NTFS_RECOVER | NTFS_READ_ONLY);
						if (mountCount <= 0) continue;

						DIR_ITER *pdir = ps3ntfs_diropen(isDir(d_path) ? d_path : cwd);
						if(pdir!=NULL)
						//{
							struct stat st; CellFsDirent entry;
							while(ps3ntfs_dirnext(pdir, entry.d_name, &st) == 0)
#else
						if(cellFsOpendir( (isDir(d_path) ? d_path : cwd), &fd) == CELL_FS_SUCCEEDED)
						{
							ssend(conn_s_ftp, FTP_OK_150); // File status okay; about to open data connection.

							bool is_root = (strlen(d_path) < 6);

							CellFsDirent entry; u64 read_e;
							u16 slen; mode_t mode; char dirtype[2]; dirtype[1] = '\0';

							while((cellFsReaddir(fd, &entry, &read_e) == CELL_FS_SUCCEEDED) && (read_e > 0))
#endif
							{
								if(IS(entry.d_name, "app_home") || IS(entry.d_name, "host_root")) continue;

								absPath(filename, entry.d_name, d_path);

								cellFsStat(filename, &buf); mode = buf.st_mode;
								cellRtcSetTime_t(&rDate, buf.st_mtime);

								if(nolist)
								{

									if(entry.d_name[0] == '.' && entry.d_name[1] == '\0')
									{
										dirtype[0] = 'c';
									}
									else
									if(entry.d_name[0] == '.' && entry.d_name[1] == '.' && entry.d_name[2] == '\0')
									{
										dirtype[0] = 'p';
									}
									else
									{
										dirtype[0] = '\0';
									}

									slen = sprintf(buffer, "%stype=%s%s;siz%s=%llu;modify=%04i%02i%02i%02i%02i%02i;UNIX.mode=0%i%i%i;UNIX.uid=root;UNIX.gid=root; %s\r\n",
											is_MLSD ? "" : " ",
											dirtype,
											((mode & S_IFDIR) != 0) ? "dir" : "file",
											((mode & S_IFDIR) != 0) ? "d" : "e", (unsigned long long)buf.st_size, rDate.year, rDate.month, rDate.day, rDate.hour, rDate.minute, rDate.second,
											(((mode & S_IRUSR) != 0) * 4 + ((mode & S_IWUSR) != 0) * 2 + ((mode & S_IXUSR) != 0) * 1),
											(((mode & S_IRGRP) != 0) * 4 + ((mode & S_IWGRP) != 0) * 2 + ((mode & S_IXGRP) != 0) * 1),
											(((mode & S_IROTH) != 0) * 4 + ((mode & S_IWOTH) != 0) * 2 + ((mode & S_IXOTH) != 0) * 1),
											entry.d_name);
								}
								else
									slen = sprintf(buffer, "%s%s%s%s%s%s%s%s%s%s   1 root  root        %llu %s %02i %02i:%02i %s\r\n",
											(mode & S_IFDIR) ? "d" : "-",
											(mode & S_IRUSR) ? "r" : "-",
											(mode & S_IWUSR) ? "w" : "-",
											(mode & S_IXUSR) ? "x" : "-",
											(mode & S_IRGRP) ? "r" : "-",
											(mode & S_IWGRP) ? "w" : "-",
											(mode & S_IXGRP) ? "x" : "-",
											(mode & S_IROTH) ? "r" : "-",
											(mode & S_IWOTH) ? "w" : "-",
											(mode & S_IXOTH) ? "x" : "-",
											(unsigned long long)buf.st_size, smonth[rDate.month-1], rDate.day,
											rDate.hour, rDate.minute, entry.d_name);

								if(send(data_s, buffer, slen, 0) < 0) break;
								sys_timer_usleep(1000);
							}

							cellFsClosedir(fd);

							if(is_root)
							{
								get_cpursx(cpursx); cpursx[7] = cpursx[20] = ' ';
								sprintf(buffer, "226 [/] [%s]\r\n", cpursx);
								ssend(conn_s_ftp, buffer);
							}
							else
							{
								char *slash = strchr(d_path + 1, '/');
								if(slash) slash[0] = '\0';

								get_cpursx(cpursx); cpursx[7] = cpursx[20] = ' ';
								sprintf(buffer, "226 [%s] [ %i %s %s]\r\n", d_path, (int)(get_free_space(d_path)>>20), STR_MBFREE, cpursx);
								ssend(conn_s_ftp, buffer);
							}
						}
						else
						{
							ssend(conn_s_ftp, FTP_ERROR_550);	// Requested action not taken. File unavailable (e.g., file not found, no access).
						}
					}
					else
					{
						ssend(conn_s_ftp, FTP_ERROR_425);		// Can't open data connection.
					}
				}
				else
				if(IS(cmd, "PASV"))
				{
					u8 pasv_retry = 0;
					rest = 0;
pasv_again:
					if(!p1x)
					{
						cellRtcGetCurrentTick(&pTick);
						p1x = ( ( (pTick.tick & 0xfe0000) >> 16) & 0xff) | 0x80; // use ports 32768 -> 65279 (0x8000 -> 0xFEFF)
						p2x = ( ( (pTick.tick & 0x00ff00) >>  8) & 0xff);
					}
					data_ls = slisten(getPort(p1x, p2x), 1);

					if(data_ls >= 0)
					{
						sprintf(pasv_output, "227 Entering Passive Mode (%s,%i,%i)\r\n", ip_address, p1x, p2x);
						ssend(conn_s_ftp, pasv_output);

						if((data_s = accept(data_ls, NULL, NULL)) > 0)
						{
							dataactive = 1;
						}
						else
						{
							ssend(conn_s_ftp, FTP_ERROR_451);	// Requested action aborted. Local error in processing.
						}

					}
					else
					{
						p1x=0;
						if(pasv_retry < 10)
						{
							pasv_retry++;
							goto pasv_again;
						}
						ssend(conn_s_ftp, FTP_ERROR_451);		// Requested action aborted. Local error in processing.
					}
				}
				else
				if(IS(cmd, "RETR"))
				{
					if(data_s >= 0)
					{
						if(split == 1)
						{
							absPath(filename, param, cwd);

							//if(file_exists(filename))
							{
								int rr=-4;

								if(islike(filename, "/dvd_bdvd"))
									{system_call_1(36, (uint64_t) "/dev_bdvd");} // decrypt dev_bdvd files

								if(cellFsOpen(filename, CELL_FS_O_RDONLY, &fd, NULL, 0) == CELL_FS_SUCCEEDED)
								{
									sys_addr_t sysmem = 0; size_t buffer_size = BUFFER_SIZE_FTP;

									if(sys_memory_allocate(buffer_size, SYS_MEMORY_PAGE_SIZE_64K, &sysmem) == 0)
									{
										char *buffer2 = (char*)sysmem;

										u64 read_e = 0, pos; //, write_e

										cellFsLseek(fd, rest, CELL_FS_SEEK_SET, &pos);
										rest = 0;

										//int optval = buffer_size;
										//setsockopt(data_s, SOL_SOCKET, SO_SNDBUF, &optval, sizeof(optval));

										ssend(conn_s_ftp, FTP_OK_150); // File status okay; about to open data connection.
										rr=0;

										while(working)
										{
											//sys_timer_usleep(1668);
											if(cellFsRead(fd, (void *)buffer2, buffer_size, &read_e) == CELL_FS_SUCCEEDED)
											{
												if(read_e > 0)
												{
													if(send(data_s, buffer2, (size_t)read_e, 0) < 0) {rr=-3; break;}
												}
												else
													break;
											}
											else
												{rr=-2; break;}
										}
										sys_memory_free(sysmem);
									}
									cellFsClose(fd);
								}

								if( rr == 0)
									ssend(conn_s_ftp, FTP_OK_226);		// Closing data connection. Requested file action successful (for example, file transfer or file abort).

								else if( rr == -4)
									ssend(conn_s_ftp, FTP_ERROR_550);	// Requested action not taken. File unavailable (e.g., file not found, no access).
								else
									ssend(conn_s_ftp, FTP_ERROR_451);	// Requested action aborted. Local error in processing.

							}
							//else ssend(conn_s_ftp, FTP_ERROR_550);	// Requested action not taken. File unavailable (e.g., file not found, no access).
						}
						else
						{
							ssend(conn_s_ftp, FTP_ERROR_501);			// Syntax error in parameters or arguments.
						}
					}
					else
					{
						ssend(conn_s_ftp, FTP_ERROR_425);				// Can't open data connection.
					}
				}
				else
				if(IS(cmd, "DELE"))
				{
					if(split == 1)
					{
						absPath(filename, param, cwd);

						if(cellFsUnlink(filename) == CELL_FS_SUCCEEDED)
						{
							ssend(conn_s_ftp, FTP_OK_250); // Requested file action okay, completed.
						}
						else
						{
							ssend(conn_s_ftp, FTP_ERROR_550); // Requested action not taken. File unavailable (e.g., file not found, no access).
						}
					}
					else
					{
						ssend(conn_s_ftp, FTP_ERROR_501); // Syntax error in parameters or arguments.
					}
				}
				else
				if(IS(cmd, "MKD"))
				{
					if(split == 1)
					{
						absPath(filename, param, cwd);

						if(cellFsMkdir(filename, MODE) == CELL_FS_SUCCEEDED)
						{
							sprintf(buffer, "257 \"%s\" OK\r\n", param);
							ssend(conn_s_ftp, buffer);
						}
						else
						{
							ssend(conn_s_ftp, FTP_ERROR_550); // Requested action not taken. File unavailable (e.g., file not found, no access).
						}
					}
					else
					{
						ssend(conn_s_ftp, FTP_ERROR_501); // Syntax error in parameters or arguments.
					}
				}
				else
				if(IS(cmd, "RMD"))
				{
					if(split == 1)
					{
						absPath(filename, param, cwd);

#ifndef LITE_EDITION
						if(del(filename, true) == CELL_FS_SUCCEEDED)
#else
						if(cellFsRmdir(filename) == CELL_FS_SUCCEEDED)
#endif
						{
							ssend(conn_s_ftp, FTP_OK_250); // Requested file action okay, completed.
						}
						else
						{
							ssend(conn_s_ftp, FTP_ERROR_550); // Requested action not taken. File unavailable (e.g., file not found, no access).
						}
					}
					else
					{
						ssend(conn_s_ftp, FTP_ERROR_501); // Syntax error in parameters or arguments.
					}
				}
				else
				if(IS(cmd, "STOR"))
				{
					if(data_s >= 0)
					{
						if(split == 1)
						{
							absPath(filename, param, cwd);

							int rr = FAILED;
							u64 pos = 0;

							if(cellFsOpen(filename, CELL_FS_O_CREAT|CELL_FS_O_WRONLY, &fd, NULL, 0) == CELL_FS_SUCCEEDED)
							{

								sys_addr_t sysmem = 0; size_t buffer_size = BUFFER_SIZE_FTP;;

								//for(uint8_t n = MAX_PAGES; n > 0; n--)
								//	if(sys_memory_allocate(n * _64KB_, SYS_MEMORY_PAGE_SIZE_64K, &sysmem) == 0) {buffer_size = n * _64KB_; break;}

								//if(buffer_size >= _64KB_)
								if(sys_memory_allocate(buffer_size, SYS_MEMORY_PAGE_SIZE_64K, &sysmem) == 0)
								{
									char *buffer2= (char*)sysmem;
									u64 read_e = 0;

									if(rest)
										cellFsLseek(fd, rest, CELL_FS_SEEK_SET, &pos);
									else
										cellFsFtruncate(fd, 0);

									rest = 0;
									rr = 0;

									ssend(conn_s_ftp, FTP_OK_150); // File status okay; about to open data connection.

									//int optval = buffer_size;
									//setsockopt(data_s, SOL_SOCKET, SO_RCVBUF, &optval, sizeof(optval));
									while(working)
									{
										//sys_timer_usleep(1668);
										if((read_e = (u64)recv(data_s, buffer2, buffer_size, MSG_WAITALL)) > 0)
										{
											if(cellFsWrite(fd, buffer2, read_e, NULL) != CELL_FS_SUCCEEDED) {rr=FAILED;break;}
										}
										else
											break;
									}
									sys_memory_free(sysmem);
								}
								cellFsClose(fd);
								cellFsChmod(filename, MODE);
								if(!working || rr!=0) cellFsUnlink(filename);
							}

							if(rr == 0)
							{
								ssend(conn_s_ftp, FTP_OK_226);		// Closing data connection. Requested file action successful (for example, file transfer or file abort).
							}
							else
							{
								ssend(conn_s_ftp, FTP_ERROR_451);	// Requested action aborted. Local error in processing.
							}
						}
						else
						{
							ssend(conn_s_ftp, FTP_ERROR_501);		// Syntax error in parameters or arguments.
						}
					}
					else
					{
						ssend(conn_s_ftp, FTP_ERROR_425);			// Can't open data connection.
					}
				}
				else
				if(IS(cmd, "SIZE"))
				{
					if(split == 1)
					{
						absPath(filename, param, cwd);
						if(cellFsStat(filename, &buf) == CELL_FS_SUCCEEDED)
						{
							sprintf(buffer, "213 %llu\r\n", (unsigned long long)buf.st_size);
							ssend(conn_s_ftp, buffer);
							dataactive = 1;
						}
						else
						{
							ssend(conn_s_ftp, FTP_ERROR_550); // Requested action not taken. File unavailable (e.g., file not found, no access).
						}
					}
					else
					{
						ssend(conn_s_ftp, FTP_ERROR_501); // Syntax error in parameters or arguments.
					}
				}
				else
				if(IS(cmd, "SYST"))
				{
					ssend(conn_s_ftp, "215 UNIX Type: L8\r\n");
				}
				else
				if(IS(cmd, "MDTM"))
				{
					if(split == 1)
					{
						absPath(filename, param, cwd);
						if(cellFsStat(filename, &buf) == CELL_FS_SUCCEEDED)
						{
							cellRtcSetTime_t(&rDate, buf.st_mtime);
							sprintf(buffer, "213 %04i%02i%02i%02i%02i%02i\r\n", rDate.year, rDate.month, rDate.day, rDate.hour, rDate.minute, rDate.second);
							ssend(conn_s_ftp, buffer);
						}
						else
						{
							ssend(conn_s_ftp, FTP_ERROR_550);	// Requested action not taken. File unavailable (e.g., file not found, no access).
						}
					}
					else
					{
						ssend(conn_s_ftp, FTP_ERROR_501);		// Syntax error in parameters or arguments.
					}
				}
				else
				if(IS(cmd, "ABOR"))
				{
					sclose(&data_s);
					ssend(conn_s_ftp, FTP_OK_ABOR_226);			// Closing data connection. Requested file action successful
				}

				else
				if(IS(cmd, "RNFR"))
				{
					if(split == 1)
					{
						absPath(source, param, cwd);

						if(file_exists(source))
						{
							ssend(conn_s_ftp, FTP_OK_RNFR_350);		// Requested file action pending further information
						}
						else
						{
							source[0] = NULL;
							ssend(conn_s_ftp, FTP_ERROR_RNFR_550);	// Requested action not taken. File unavailable
						}
					}
					else
					{
						source[0] = NULL;
						ssend(conn_s_ftp, FTP_ERROR_501);			// Syntax error in parameters or arguments.
					}
				}

				else
				if(IS(cmd, "RNTO"))
				{
					if(split == 1 && source[0]=='/')
					{
						absPath(filename, param, cwd);

						if(cellFsRename(source, filename) == CELL_FS_SUCCEEDED)
						{
							ssend(conn_s_ftp, FTP_OK_250); // Requested file action okay, completed.
						}
						else
						{
							ssend(conn_s_ftp, FTP_ERROR_550); // Requested action not taken. File unavailable (e.g., file not found, no access).
						}
					}
					else
					{
						ssend(conn_s_ftp, FTP_ERROR_501); // Syntax error in parameters or arguments.
					}
					source[0]=0;
				}

				else
				if(IS(cmd, "USER") || IS(cmd, "PASS"))
				{
					ssend(conn_s_ftp, FTP_OK_USER_230); // User logged in, proceed.
				}
				else
				/*if(  IS(cmd, "OPTS") == 0
					|| IS(cmd, "REIN") || IS(cmd, "ADAT")
					|| IS(cmd, "AUTH") || IS(cmd, "CCC" )
					|| IS(cmd, "CONF") || IS(cmd, "ENC" )
					|| IS(cmd, "EPRT") || IS(cmd, "EPSV")
					|| IS(cmd, "LANG") || IS(cmd, "LPRT")
					|| IS(cmd, "LPSV") || IS(cmd, "MIC" )
					|| IS(cmd, "PBSZ") || IS(cmd, "PROT")
					|| IS(cmd, "SMNT") || IS(cmd, "STOU")
					|| IS(cmd, "XRCP") || IS(cmd, "XSEN")
					|| IS(cmd, "XSEM") || IS(cmd, "XRSQ")
					|| IS(cmd, "STAT"))
				{
					ssend(conn_s_ftp, FTP_ERROR_502);	// Command not implemented.
				}
				else*/
				{
					ssend(conn_s_ftp, FTP_ERROR_500);	// Syntax error, command unrecognized and the requested	action did not take place.
				}

				if(dataactive == 1)
				{
					dataactive = 0;
				}
				else
				{
					sclose(&data_s);
					if(data_ls > 0) {sclose(&data_ls); data_ls=FAILED;}
					rest = 0;
				}
			}
			else if (working)
			{
				// commands available when not logged in
				if(IS(cmd, "USER"))
				{
					if(split == 1)
					{
						ssend(conn_s_ftp, FTP_OK_331); // User name okay, need password.
					}
					else
					{
						ssend(conn_s_ftp, FTP_ERROR_501); // Syntax error in parameters or arguments.
					}
				}
				else
				if(IS(cmd, "PASS"))
				{
					if(split == 1)
					{
						if((webman_config->ftp_password[0] == NULL) || IS(webman_config->ftp_password, param))
						{
							ssend(conn_s_ftp, FTP_OK_230);		// User logged in, proceed. Logged out if appropriate.
							loggedin = 1;
						}
						else
						{
							ssend(conn_s_ftp, FTP_ERROR_430);	// Invalid username or password
						}
					}
					else
					{
						ssend(conn_s_ftp, FTP_ERROR_501);		// Syntax error in parameters or arguments.
					}
				}
				else
				if(IS(cmd, "QUIT") || IS(cmd, "BYE"))
				{
					ssend(conn_s_ftp, FTP_OK_221); // Service closing control connection.
					connactive = 0;
				}
				else
				{
					ssend(conn_s_ftp, FTP_ERROR_530); // Not logged in.
				}
			}
			else
			{
				connactive = 0;
				loggedin = 0;
				break;
			}

		}
		else
		{
			connactive = 0;
			loggedin = 0;
			break;
		}

		sys_timer_usleep(1668);
	}

	sclose(&conn_s_ftp);
	sclose(&data_s);

	sys_ppu_thread_exit(0);
}

static void ftpd_thread(uint64_t arg)
{
	int list_s = FAILED;

relisten:
	if(working) list_s = slisten(FTPPORT, 4);
	else goto end;

	if(working && (list_s < 0))
	{
		sys_timer_sleep(3);
		if(working) goto relisten;
		else goto end;
	}

	//if(list_s >= 0)
	{
		while(working)
		{
			sys_timer_usleep(1668);
			int conn_s_ftp;
			if(!working) break;
			else
			if(working &&(conn_s_ftp = accept(list_s, NULL, NULL)) > 0)
			{
				sys_ppu_thread_t id;
				if(working) sys_ppu_thread_create(&id, handleclient_ftp, (u64)conn_s_ftp, THREAD_PRIO_FTP, THREAD_STACK_SIZE_8KB, SYS_PPU_THREAD_CREATE_NORMAL, THREAD_NAME_FTPD);
				else {sclose(&conn_s_ftp); break;}
			}
			else
			if((sys_net_errno==SYS_NET_EBADF) || (sys_net_errno==SYS_NET_ENETDOWN))
			{
				sclose(&list_s);
				list_s = FAILED;
				if(working) goto relisten;
				else break;
			}
		}
	}
end:
	sclose(&list_s);
	sys_ppu_thread_exit(0);
}
