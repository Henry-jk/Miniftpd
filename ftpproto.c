#include "ftpproto.h"
#include "sysutil.h"
#include "str.h"
#include "ftpcodes.h"
#include "tunable.h"
#include "privsock.h"
#include "session.h"

void start_cmdio_alarm();
void start_data_alarm();

void handle_sigalrm(int sig);
void handle_alarm_timeout(int sig);
void handle_sigurg(int sig);

void check_abor(session_t *sess);
 //����ǰĿ¼�г���
int list_common(session_t *sess,int detail); 
void upload_common(session_t *sess,int is_append);
void limit_rate(session_t *sess, int bytes_transfered, int is_upload);


int get_transferfd(session_t *sess);//���ش������������ӵ��׽���
int port_active(session_t *sess);
int pasv_active(session_t *sess);
int get_port_fd(session_t *sess);
int get_pasv_fd(session_t *sess);

void ftp_reply(session_t *sess,int status,const char *text);
void ftp_lreply(session_t *sess,int status,const char *text);
static void do_user(session_t *sess);//��֤�û�
static void do_pass(session_t *sess);//��֤����
static void do_syst(session_t *sess); //��Ӧ�ͻ��˵�ǰϵͳ������
static void do_feat(session_t *sess);//����������˵�����
static void do_pwd(session_t *sess); //��ʾ��ǰ��Ŀ¼
static void do_type(session_t *sess);//����ģʽ�Ƿ���ASCIIģʽ
static void do_port(session_t *sess);
static void do_list(session_t *sess);
static void do_pasv(session_t *sess);
static void do_nlst(session_t *sess);
static void do_cwd(session_t *sess);
static void do_cdup(session_t *sess);
static void do_mkd(session_t *sess);//����Ŀ¼(�ļ���)
static void do_rmd(session_t *sess);//ɾ��Ŀ¼(�ļ���)
static void do_dele(session_t *sess);//ɾ���ļ�
static void do_rest(session_t *sess);
static void do_size(session_t *sess);
//�������������ǽ����ļ�����������  �����ȷ�rnfr �ٷ�rnto
static void do_rnfr(session_t *sess);
static void do_rnto(session_t *sess);

static void do_retr(session_t *sess);//�����ļ�   �ϵ�����
static void do_stor(session_t *sess);//�ϴ��ļ�   �ϵ�����

static void do_noop(session_t *sess);
static void do_quit(session_t *sess);
static void do_abor(session_t *sess);
/*static void do_stru(session_t *sess);
//static void do_mode(session_t *sess);
static void do_appe(session_t *sess);


static void do_site(session_t *sess);
static void do_stat(session_t *sess);
static void do_help(session_t *sess);
//FTP���������������Ӧ��
static void do_site_chmod(session_t *sess, char *chmod_arg);
static void do_site_umask(session_t *sess, char *umask_arg);*/

typedef struct ftpcmd {
	const char *cmd; //�����ַ���
	void (*cmd_handler)(session_t *sess);
} ftpcmd_t;

static ftpcmd_t ctrl_cmds[] = {
	/* ���ʿ������� */
	{"USER",	do_user	},
	{"PASS",	do_pass	},
	{"SYST",	do_syst },
	{"FEAT",	do_feat },//���Ҳ�ʵ��do_feat()����
	{"PWD" ,	do_pwd	},
	{"TYPE",	do_type	},
	{"PORT",	do_port },
	{"LIST",	do_list },
	{"PASV",	do_pasv },
	{"NLST",	do_nlst },
	{"CWD" ,    do_cwd  },
	{"CDUP",	do_cdup },
	{"MKD" ,	do_mkd	},
	{"RMD" ,	do_rmd	},
	{"DELE",	do_dele },
	{"REST",	do_rest },
	{"SIZE",	do_size },
	{"RNFR",	do_rnfr },
	{"RNTO",	do_rnto },
	{"RETR",	do_retr },
	{"RNTO",	do_rnto	},
	{"STOR",	do_stor },
	{"NOOP",	do_noop	},
	{"QUIT",	do_quit	},
	{"ABOR",	do_abor	},
	{"\377\364\377\362ABOR",do_abor}
};
//
//�ӿͻ���һ��һ�еؽ�������
/*
�ͻ��˷��͹���������˳���ǲ�ȷ����
*/
session_t *p_sess;

void check_abor(session_t *sess)
{
	if (sess->abor_received)
	{
		sess->abor_received=0;
		ftp_reply(p_sess, FTP_ABOROK, "ABOR successful.");
	}
}

void handle_alarm_timeout(int sig)
{
	shutdown(p_sess->ctrl_fd,SHUT_RD);
	ftp_reply(p_sess,FTP_IDLE_TIMEOUT,"Timeout.");
	shutdown(p_sess->ctrl_fd,SHUT_WR);
	exit(EXIT_FAILURE);
}

void start_cmdio_alarm()
{
	if(tunable_idle_session_timeout>0)
	{
		//��װ�ź�
		signal(SIGALRM,handle_alarm_timeout);
		//��������
		alarm(tunable_idle_session_timeout);
	}
}

void handle_sigalrm(int sig)
{
	if (!p_sess->data_process)
	{
		ftp_reply(p_sess, FTP_DATA_TIMEOUT, "Data timeout. Reconnect. Sorry.");
		exit(EXIT_FAILURE);
	}

	// ���򣬵�ǰ�������ݴ����״̬�յ��˳�ʱ�ź�
	p_sess->data_process = 0;
	start_data_alarm();
}

//һ��������SIGURG�źţ���ζ�ŷ�����һ����������
void handle_sigurg(int sig)
{
	if (p_sess->data_fd==-1)
		return ;
	//������������״̬
	//char cmdline[MAX_COMMAND_LINE]={0};
	//���н���
	int ret = readline(p_sess->ctrl_fd,p_sess->cmdline,MAX_COMMAND_LINE);
	if(ret<=0)
		ERR_EXIT("readline");
	//��������
	/*����ȥ��\r\n
	*/
	str_trim_crlf(p_sess->cmdline);
	if( strcmp(p_sess->cmdline,"\377\364\377\362ABOR")==0
		|| strcmp(p_sess->cmdline,"ABOR")==0 )
	{
		p_sess->abor_received=1; //�ñ��������Ƿ��յ�abor����
		shutdown(p_sess->data_fd,SHUT_RDWR);//�Ͽ���������ͨ��
	}
	else
		ftp_reply(p_sess, FTP_BADCMD, "Unknown command.");

}
void start_data_alarm()
{
	if (tunable_data_connection_timeout>0)
	{
		//��װ�ź�
		signal(SIGALRM,handle_sigalrm);
		//��������
		alarm(tunable_data_connection_timeout);
	}
	else if(tunable_idle_session_timeout>0)
	{
		//�ر���ǰ������
		alarm(0);
	}
}
void handle_child(session_t *sess)
{
	ftp_reply(sess,FTP_GREET,"(miniftpd 0.1)");
	while(1)
	{
		memset(sess->cmdline,0,MAX_COMMAND_LINE);
		memset(sess->cmd,0,MAX_COMMAND);
		memset(sess->arg,0,MAX_ARG);

		start_cmdio_alarm();

		int ret=readline(sess->ctrl_fd,sess->cmdline,MAX_COMMAND_LINE);
		if(ret==-1)
			ERR_EXIT("readline");//�ر�ftp�������   ��ʱҲҪ�ر�nobody����
		else if(ret==0)//��ʾ�ͻ��˶Ͽ�������
			exit(EXIT_SUCCESS);//�ر�ftp�������     ��ʱҲҪ�ر�nobody����
		printf("cmdline=[%s]\n",sess->cmdline);
		//ȥ��\r\n
		str_trim_crlf(sess->cmdline);
		printf("cmdline=[%s]\n",sess->cmdline);
		//�������������-----FTP����
		str_split(sess->cmdline,sess->cmd,sess->arg,' ');
		printf("cmd=[%s] arg=[%s]\n",sess->cmd,sess->arg);
		//������ת���ɴ�д
		str_upper(sess->cmd);
		//����FTP����---������Ҫnobody���̵�Э�� ��ʱ�������Ҫ��nobody���̷����ڲ�����

	/*	if(strcmp("USER",sess->cmd)==0)
		{
			do_user(sess);
		}
		else if(strcmp("PASS",sess->cmd)==0)
		{
			do_pass(sess);
		}*/
		//FTP�����ӳ��
		int i;
		int size=sizeof(ctrl_cmds)/sizeof(ctrl_cmds[0]);
		for(i=0;i<size;i++)
		{
			if(strcmp(ctrl_cmds[i].cmd,sess->cmd)==0)//˵������ƥ��
			{
				if(ctrl_cmds[i].cmd_handler!=NULL)
					ctrl_cmds[i].cmd_handler(sess);
				else	//˵������δʵ��
					ftp_reply(sess,FTP_COMMANDNOTIMPL,"UnImplement Command.");
				break;
			}
		}
		if(i==size)
		{
			//˵��δ�ҵ�����
			ftp_reply(sess,FTP_BADCMD,"Unknown Command.");
		}
	}
}

//��֤�û�
static void do_user(session_t *sess)
{
	//USER jjl
	struct passwd *pw=getpwnam(sess->arg);
	if(pw==NULL)//�ܿ����ǲ����ڵ��û�
	{
		ftp_reply(sess,FTP_LOGINERR,"Login incorrect.");//530
		return ;
	}
	sess->uid=pw->pw_uid;//������û���id
	ftp_reply(sess,FTP_GIVEPWORD,"Please specify the password.");//331
}

//��֤����
static void do_pass(session_t *sess)
{
	//PASS 123456(����) ʵ�ʵ������Ǳ�����Ӱ���ļ���
	struct passwd *pw=getpwuid(sess->uid); //�����û�id�õ�����ṹ�塶====����ȡ�����ļ���Ϣ
	
	if(pw==NULL)//�ܿ����ǲ����ڵ��û�
	{
		ftp_reply(sess,FTP_LOGINERR,"Login incorrect."); //530
		return ;
	}
	//printf("username=%s\n",pw->pw_name);
	struct spwd *sp=getspnam(pw->pw_name);//�����û��� ��ȡӰ���ļ���Ϣ
	if(sp==NULL)
	{
		ftp_reply(sess,FTP_LOGINERR,"Login incorrect."); //530
		return ;
	}
	//���Ƚ���������(123456)
	char *encrypt_pass=crypt(sess->arg,sp->sp_pwdp);//encrypt_pass����õ��Ѽ��ܵ�����
	//�����������֤
	if(strcmp(encrypt_pass,sp->sp_pwdp)!=0)//������֤ʧ��
	{
		ftp_reply(sess,FTP_LOGINERR,"Login incorrect."); //530
		return ;
	}

	signal(SIGURG,handle_sigurg);
	activate_sigurg(sess->ctrl_fd);

	umask(tunable_local_umask);
	//������֤�ɹ���ʱ��  �����������
	setgid(pw->pw_gid);//����Ϊʵ���û�����id
	setuid(pw->pw_uid);//����Ϊʵ���û����û�id
	chdir(pw->pw_dir);//����Ϊʵ���û��ļ�Ŀ¼
	ftp_reply(sess,FTP_LOGINOK,"Login successfully."); //230
}
static void do_syst(session_t *sess)
{ 
	ftp_reply(sess,FTP_SYSTOK,"UNIX Type: L8");
}

void ftp_reply(session_t *sess,int status,const char *text)
{
	char buf[1024]={0};
	sprintf(buf,"%d %s\r\n",status,text);
	writen(sess->ctrl_fd,buf,strlen(buf));////////////+1  �ͻ��˾Ͳ��ᷢ��PASS����
}


static void do_feat(session_t *sess)
{
	ftp_lreply(sess,FTP_FEAT,"Features:");
	writen(sess->ctrl_fd,"EPRT\r\n",strlen("EPRT\r\n"));
	writen(sess->ctrl_fd,"EPSV\r\n",strlen("EPSV\r\n"));
	writen(sess->ctrl_fd,"MDTM\r\n",strlen("MDTM\r\n"));
	writen(sess->ctrl_fd,"PASV\r\n",strlen("PASV\r\n"));
	writen(sess->ctrl_fd,"REST STREAM\r\n",strlen("REST STREAM\r\n"));
	//REST STREAM:˵���÷�����֧�ֶϵ�����  ��������Ϣ֮�� ftp�ͻ��˽����ᷢ��REST����
	writen(sess->ctrl_fd,"SIZE\r\n",strlen("SIZE\r\n"));
	writen(sess->ctrl_fd,"TVFS\r\n",strlen("TVFS\r\n"));
	writen(sess->ctrl_fd,"UTF8\r\n",strlen("UTF8\r\n"));
	ftp_reply(sess,FTP_FEAT,"End");
}
static void do_pwd(session_t *sess)
{
	char text[1024]={0};
	char dir[1024+1]={0};
	//��ȡ��ǰĿ¼
	getcwd(dir,1024);
	//printf("%s\n",dir);
	sprintf(text,"\"%s\"",dir);
	ftp_reply(sess,FTP_PWDOK,text);
}
static void do_type(session_t *sess)
{
	if(strcmp(sess->arg,"A")==0)
	{
		sess->is_ascii=1;
		ftp_reply(sess,FTP_TYPEOK,"Switching to ASCII mode.");
	}
	else if(strcmp(sess->arg,"I")==0)
	{
		sess->is_ascii=0;
		ftp_reply(sess,FTP_TYPEOK,"Switching to Binary mode.");
	}
	else
		ftp_reply(sess,FTP_BADCMD,"Unrecognised TYPE command."); 
}
void ftp_lreply(session_t *sess,int status,const char *text)
{
	char buf[1024]={0};
	sprintf(buf,"%d-%s\r\n",status,text);
	writen(sess->ctrl_fd,buf,strlen(buf));////////////+1  �ͻ��˾Ͳ��ᷢ��PASS����
}

int list_common(session_t *sess,int detail)
{
	DIR *dir=opendir(".");//�򿪵�ǰĿ¼
	if(dir==NULL)
		return 0;
	//������ǰĿ¼�е��ļ� readdir()����
	struct dirent *dt;
	struct stat sbuf;
	char buf[1024]={0};
	while( (dt=readdir(dir))!=NULL)
	{
		if(lstat(dt->d_name,&sbuf)<0)//��ȡ�ļ���״̬  ��ȡһ���ļ���������һ��������
			continue;//�����ȡ�ļ�ʧ�� ��continue  ������һ���ļ�
		if(dt->d_name[0]=='.')
			continue;
		/*lstat()  is  identical to stat(), except that if path is a symbolic link, then the link itself is stat-ed,
		 not the file that it refers to.*/
		 if(detail) //��ϸ�嵥
		 {
			 const char *perms=statbuf_get_perms(&sbuf);
			
			 int off=0;
			 off += sprintf(buf,"%s ",perms);
			 off += sprintf(buf+off," %3d %-8d %-8d ",(int)sbuf.st_nlink,sbuf.st_uid,sbuf.st_gid);
			 off += sprintf(buf+off," %8lu  ",(unsigned long)sbuf.st_size);
																//��ǰϵͳ��ʱ��(NULL)
			 const char *datebuf=statbuf_get_date(&sbuf);
			 off += sprintf(buf+off,"%s ",datebuf);
			 if(S_ISLNK(sbuf.st_mode))
			 {
				 char temp[64]={0};
				 readlink(dt->d_name,temp,sizeof(temp));//��ȡ�����ļ���ָ�ļ� �����ļ���������temp��
				 sprintf(buf+off,"%s -> %s\r\n",dt->d_name,temp);
			 }
			 else
				sprintf(buf+off,"%s\r\n",dt->d_name);
		 }
		 else
			 sprintf(buf,"%s\r\n",dt->d_name);

		 writen(sess->data_fd,buf,strlen(buf)); //���͸�ftp�ͻ���
		// printf("%s",buf);//��ӡ����׼���
	}
	//�ر�Ŀ¼
	closedir(dir);
	return 1;
}

//˯��ʱ��=(��ǰ�����ٶ�/������ٶ�-1)*��ǰ����ʱ��
//����ʵ��
void limit_rate(session_t *sess, int bytes_transfered, int is_upload) //bytes_transfered:��ǰ������ֽ���
{
	sess->data_process=1;
	//������һ���������� �ٴλ�ȡ��ǰ��ʱ��
	long curr_sec = get_time_sec();
	long curr_usec = get_time_usec();

	double elapsed;//��ǰ����ʱ��
	elapsed = (double)(curr_sec - sess->bw_transfer_start_sec);
	elapsed += (double)(curr_usec - sess->bw_transfer_start_usec) / (double)1000000;
	if (elapsed <= (double)0) 
	{
		elapsed = (double)0.01;
	}
	
	// ���㵱ǰ�����ٶ�
	unsigned int bw_rate = (unsigned int)((double)bytes_transfered / elapsed);
	double rate_ratio;
	if (is_upload) 
	{
		if (bw_rate <= sess->bw_upload_rate_max) 
		{
			// ����Ҫ����
			sess->bw_transfer_start_sec = curr_sec;
			sess->bw_transfer_start_usec = curr_usec;
			return;
		}
		rate_ratio = bw_rate / sess->bw_upload_rate_max;
	} 
	else 
	{
		if (bw_rate <= sess->bw_download_rate_max) 
		{
			// ����Ҫ����
			sess->bw_transfer_start_sec = curr_sec;
			sess->bw_transfer_start_usec = curr_usec;
			return;
		}
		rate_ratio = bw_rate / sess->bw_download_rate_max;
	}

	// ˯��ʱ�� = (��ǰ�����ٶ� / ������ٶ� �C 1) * ��ǰ����ʱ��;
	double pause_time;	// ˯��ʱ��
	pause_time = (rate_ratio - (double)1) * elapsed;

	nano_sleep(pause_time);

	sess->bw_transfer_start_sec = get_time_sec();
	sess->bw_transfer_start_usec = get_time_usec();
}

void upload_common(session_t *sess,int is_append)
{
	//�ϴ��ļ�   �ϵ�����   �ͻ������Ȼᷢ��PASV or PORT����
	//������������
	if(get_transferfd(sess)==0)//������������ʧ��	
		return ;
	long long offset=sess->restart_pos;
	sess->restart_pos=0;

	//�ȴ�Ҫ�ϴ����ļ�          
	int fd=open(sess->arg,O_CREAT|O_WRONLY,0666);
	if(fd==-1)
	{
		ftp_reply(sess,FTP_UPLOADFAIL,"Could not create file.");
		return ;
	}
	//��Ҫ�򿪵��ļ���д��    ԭ��:���ϴ��ļ���ʱ�� ��������˶����ļ� �����������д����ļ�
	int ret=lock_file_write(fd);
	if(ret==-1)
	{
		ftp_reply(sess,FTP_FILEFAIL,"Lock file failed.");
		return ;
	}

	//lseek() ���������ļ�������fd�����Ĵ��ļ���ƫ�������¶�λ������ƫ����
	if(is_append!=0&& offset==0 )  //STOR
	{
		ftruncate(fd,0);//���ļ����㣿������������������������
		lseek(fd,offset,SEEK_SET);//SEEK_SET :ƫ��������Ϊƫ���ֽ�
	}
	else if(is_append!=0 && offset!=0) //REST ---	STOR
	{
		if (lseek(fd,offset,SEEK_SET)<0)
		{
			ftp_reply(sess,FTP_FILEFAIL,"dingwei weizhi failed!");
			return ;
		}
	}

	struct stat sbuf;
	if (fstat(fd,&sbuf)<0)
	{
		printf("get file failed!");
		return ;
	}
	//150
	char text[1024];
	if(sess->is_ascii)
	{
		sprintf(text,"Opening ASCII mode data connection for %s (%lld)",
			sess->arg,(long long)sbuf.st_size);
	}
	else
	{
		sprintf(text,"Opening BINARY mode data connection for %s (%lld)",
			sess->arg,(long long)sbuf.st_size);
	}
	//150 : ��ʼ�����ļ�
	ftp_reply(sess,FTP_DATACONN,text);//150

	//�ϴ��ļ�
	int flag=0;//��־λ ��ʾ��Ӧ�ļ������
	char buf[1024]={0};

	/*��ʼ�����ʱ��*/
	sess->bw_transfer_start_sec=get_time_sec();//��ȡ��ǰʱ�������
	sess->bw_transfer_start_usec=get_time_usec();//��ȡ��ǰʱ���΢����

	while (1)
	{
		ret=read(sess->data_fd,buf,sizeof(buf));//����ֵ�� ��ǰ��ȡ���ֽ���
		if(ret==-1)
		{
			if(errno==EINTR)
				continue;
			else
			{
				flag=2;//��ʾ����
				break;
			}
		}
		else if(ret==0)//��ʾ��ȡ�����ļ�ĩβ  �ͻ��˶Ͽ�������
		{
			flag=0;
			break;
		}

		limit_rate(sess,ret,1);
		if (sess->abor_received)
		{
			flag=1;
			break;
		}
	   //��ȡ����һ��������  д�������׽�����
		if (writen(fd,buf,ret) != ret)
		{
			flag=1;
			break;
		}
	}
	//�ر������׽���
	close(sess->data_fd);
	sess->data_fd=-1;
	close(fd);
	if(flag==0)
	{
		//226
		ftp_reply(sess,FTP_TRANSFEROK,"Transfer complete.");
	}
	else if(flag==1)
	{
		//426
		ftp_reply(sess,FTP_BADSENDNET,"Failure writing to local file.");
	}
	else if(flag==2)
	{
		//451
       ftp_reply(sess,FTP_BADSENDFILE,"Failure reading from network stream .");
	}

	start_cmdio_alarm();//���¿�����������ͨ������  ԭ���� ֮ǰ�������п��ܹر�
}
//�ڴ�����������ͨ��֮ǰ  ����ҪЭ��ʹ��PORT����ʹ��PASVģʽ
static void do_port(session_t *sess)
{
	//PORT 192,168,127,1,253,127
	unsigned int v[6];
	sscanf(sess->arg,"%u,%u,%u,%u,%u,%u",&v[2],&v[3],&v[4],&v[5],&v[0],&v[1]);
	sess->port_addr=(struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
	memset(sess->port_addr,0,sizeof(struct sockaddr_in));
	sess->port_addr->sin_family=AF_INET;
	unsigned char *p=(unsigned char *)&(sess->port_addr->sin_port);
	p[0]=v[0];
	p[1]=v[1];
	//printf("%d\n",ntohs(sess->port_addr->sin_port));
	p=(unsigned char*)&(sess->port_addr->sin_addr);
	p[0]=v[2];
	p[1]=v[3];
	p[2]=v[4];
	p[3]=v[5];
	printf("%s\n",inet_ntoa(sess->port_addr->sin_addr));
	ftp_reply(sess,FTP_PORTOK,"command successful. Consider using PASV.");//200
}
int pasv_active(session_t *sess)
{
	
/*	if(sess->pasv_listenfd!=-1)
	{
		if(sess->port_addr!=NULL)
		{
			fprintf(stderr,"both port and pasv are active!\n");
			exit(EXIT_FAILURE);
		}
		return 1;
	}
	return 0;*/
	

	priv_sock_send_int(sess->child_fd,PRIV_SOCK_PASV_ACTIVE);
	int active=priv_sock_get_int(sess->child_fd);
	//printf("%d\n",active);
	if(active)
	{
		if(sess->port_addr!=NULL)
		{
			fprintf(stderr,"both port and pasv are active!\n");
			exit(EXIT_FAILURE);
		}
		return 1;
	}
	return 0;
}

static void do_pasv(session_t *sess)
{
 // Entering Passive Mode (192,168,76,6,75,175).
	 char ip[16]; 
	// getlocalip(ip);//��ȡ����ip��ַ   // printf("%s\n",ip);
	
	 priv_sock_send_cmd(sess->child_fd,PRIV_SOCK_PASV_LISTEN);//��nobody���̻�ȡpasvģʽ�ļ����˿�
	 unsigned short port=priv_sock_get_int(sess->child_fd);
	
	 strcpy(ip,"192.168.76.6");//getlocalip(ip);
	 unsigned int v[4];//ip��ַ
	 sscanf(ip,"%u.%u.%u.%u",&v[0],&v[1],&v[2],&v[3]);
	 char text[1024]={0};
	 sprintf(text,"Entering Passive Mode (%u,%u,%u,%u,%u,%u).",v[0],v[1],v[2],v[3],port>>8,port & 0xFF);
	 ftp_reply(sess,FTP_PASVOK,text);
}
int port_active(session_t *sess)
{
	if(sess->port_addr!=NULL)  //����ģʽ����ͬʱ���ڼ���״̬
	{
		if(pasv_active(sess))
		{
			fprintf(stderr,"both port and pasv are active!\n");
			exit(EXIT_FAILURE);
		}
		return 1;
	}
	return 0;
}
int get_port_fd(session_t *sess)
{		
	/*
		FTP������̽���PORT h1,h2,h3,h4,p1,p2---------------
		���� ip port----------------------------------------ǰ2������do_port��������ʵ��

		��nobody���̷���PRIV_SOCK_GET_DATA_SOCK����	1
		��nobody���̷���һ������port				4
		��nobody���̷���һ���ַ��� ip				������    
	*/
		unsigned short port=ntohs(sess->port_addr->sin_port);
		char *ip=inet_ntoa(sess->port_addr->sin_addr);
		priv_sock_send_cmd(sess->child_fd,PRIV_SOCK_GET_DATA_SOCK);
		priv_sock_send_int(sess->child_fd,(int)port);
		priv_sock_send_buf(sess->child_fd,ip,strlen(ip));
		///////////////////////////////////////////////////////
		char res = priv_sock_get_result(sess->child_fd);
		///////////////////////////////////////////////////////
		if(res==PRIV_SOCK_RESULT_BAD) //�յ�ʧ�ܵ�Ӧ��-----��˵��nobody���̴�����������ͨ��ʧ��
			return 0;//ʧ��
		else if(res==PRIV_SOCK_RESULT_OK)
			sess->data_fd=priv_sock_recv_fd(sess->child_fd);  //����ɹ��������׽���
		return 1; //����1 �ɹ�
}

int get_pasv_fd(session_t *sess)//��ȡ����ģʽ�������׽���
{
	priv_sock_send_cmd(sess->child_fd,PRIV_SOCK_PASV_ACCEPT);
	char res = priv_sock_get_result(sess->child_fd);
	if(res==PRIV_SOCK_RESULT_BAD) //�յ�ʧ�ܵ�Ӧ��-----��˵��nobody���̴�����������ͨ��ʧ��
			return 0;//ʧ��
		else if(res==PRIV_SOCK_RESULT_OK)
			sess->data_fd=priv_sock_recv_fd(sess->child_fd);  //����ɹ��������׽���
	return 1; //����1 �ɹ�
}
//��������ͨ���Ĵ���������ģʽ: ����ģʽ  ����ģʽ
int get_transferfd(session_t *sess)
{
	/*
	FTP������̽���PORT h1,h2,h3,h4,p1,p2
	���� ip port
	��nobody���̷���PRIV_SOCK_GET_DATA_SOCK����	1
	��nobody���̷���һ������port				4
	��nobody���̷���һ���ַ��� ip				������    
	*/

	//����Ƿ��յ�PORT��PASV����
	if(!port_active(sess) && !pasv_active(sess))	//|| !pasv_active(sess)
	{
		ftp_reply(sess,FTP_BADSENDCONN,"Use PORT or PASV first.");
		return 0;
	}
	int ret=1;  //��retĬ����Ϊ1
	if(port_active(sess))//�����PORTģʽ
	{
	/*	socket
		bind 20 
		connect*/
	//	tcp_client(20);
/*		int fd=tcp_client(0);//sess->data_fd ���������׽���
		//printf("%s\n",inet_ntoa(sess->port_addr->sin_addr));  
		if(connect_timeout(fd, sess->port_addr, tunable_connect_timeout)<0)
		{
			close(fd);
			return 0;//���ؼ�
		}
		//���ӳɹ�
		sess->data_fd=fd;*/
		//�����ϴ����滻Ϊ ��nobody����Э��ftp���������������������ͨ��
		//��������:
		if(get_port_fd(sess)==0)   //��ȡnobody���̷��͹��������������׽���
			ret=0;  //���������׽���ʧ��    ret��Ϊ0
	}
	if (sess->port_addr!=NULL)
	{
		free(sess->port_addr);
		sess->port_addr=NULL;
	}
	if(pasv_active(sess) )	//����Ǳ���ģʽ�Ļ�
	{
		/*int fd=accept_timeout(sess->pasv_listenfd,NULL,tunable_accept_timeout);//NULL��ʾ������Ҫ�ͻ��˵ĵ�ַ��Ϣ
		close(sess->pasv_listenfd);//����accept_timeout�����Ƿ�ɹ�  ��Ҫ�رռ����׽���pasv_listenfd
		if(fd==-1)	
			return 0; 
		sess->data_fd=fd;*/
		if(get_pasv_fd(sess)==0)
			ret=0;  //���������׽���ʧ��    ret��Ϊ0
	}
	if (ret)
	{
		//���°�װ�ź� ����������  �������ϴ��������� ��Ҫ����start_data_alarm()
		start_data_alarm();
	}
	return ret;   //����ִ�е������   ˵���������������׽��ֳɹ�
}


static void do_list(session_t *sess)
{
	//������������
	if(get_transferfd(sess)==0)//������������ʧ��	
		return ;
	//150
	ftp_reply(sess,FTP_DATACONN,"Here comes the directory listing.");
	//�����б�
	list_common(sess,1);//1��ʾ�������嵥
	//226
	ftp_reply(sess,FTP_TRANSFEROK,"Directory send OK.");
	//�ر������׽���
	close(sess->data_fd);
	sess->data_fd=-1;
}

static void do_nlst(session_t *sess)
{
	//������������
	if(get_transferfd(sess)==0)//������������ʧ��	
		return ;
	//150
	ftp_reply(sess,FTP_DATACONN,"Here comes the directory listing.");
	//�����б�
	list_common(sess,0);  //0��ʾ�̵��嵥
	//226
	ftp_reply(sess,FTP_TRANSFEROK,"Directory send OK.");
	//�ر������׽���
	close(sess->data_fd);
	sess->data_fd=-1;
}

///////////////////////////////////////////////////////////////////
static void do_cwd(session_t *sess) //�ı䵱ǰĿ¼  ���߽���ĳ��Ŀ¼ ���߷����ϲ�Ŀ¼
{
	if(chdir(sess->arg)<0)//�����¼���û�Ȩ�޲���  ���failed
	{
		ftp_reply(sess,FTP_FILEFAIL,"failed to change directory."); 
		return ;
	}
	ftp_reply(sess,FTP_CWDOK,"Directory successfully changed."); 
}

static void do_cdup(session_t *sess)//������һ��Ŀ¼
{
	if(chdir("..")<0)//�����¼���û�Ȩ�޲���  ���failed
	{
		ftp_reply(sess,FTP_FILEFAIL,"failed to change directory."); //550
		return ;
	}
	ftp_reply(sess,FTP_CWDOK,"Directory successfully changed."); 
}

static void do_mkd(session_t *sess)
{
	//ʵ�ʴ������ļ���Ȩ���� 0777 & umask
	if(mkdir(sess->arg,0777)<0)//�ڵ�ǰĿ¼�´����ļ�
	{
		//ʧ�ܵ�������: ��һ��û��дȨ�޵�Ŀ¼�´���Ŀ¼��ʧ�ܣ�������������������������������
		ftp_reply(sess,FTP_FILEFAIL,"Create directory operation failed.");//550
		return ;
	}
	char text[4096]={0};
	if(sess->arg[0]=='/')//����·��
	{
		//printf("%s\n",sess->arg);
		sprintf(text,"%s created",sess->arg);
	}
	else //���·��
	{
		char dir[4096+1]={0};
		getcwd(dir,4096);		//��ȡftp�ͻ��˵�¼������ʱ�ĵ�ǰĿ¼
		if(dir[strlen(dir)-1]=='/')//dir���һ���ַ��Ƿ���/
		{
			sprintf(text,"%s%s created",dir,sess->arg);
		}
		else
			sprintf(text,"%s/%s created",dir,sess->arg);
	}
	
	ftp_reply(sess,FTP_MKDIROK,text);//550
}

//ɾ���ļ���
//���Եݹ�ɾ��ָ��Ŀ¼�µ�����Ŀ¼���ļ�  ԭ����: ftp�ͻ��˲��Ϸ��� ��ָ��Ŀ¼�µ�����Ŀ¼������ ����ʵ�ֶ�Ӧ��ɾ��
static void do_rmd(session_t *sess)
{
	if(rmdir(sess->arg)<0)
	{
		//ʧ�ܵ�������: ��һ��û��дȨ�޵�Ŀ¼��ɾ��Ŀ¼��ʧ�ܣ�������������������������������
		ftp_reply(sess,FTP_FILEFAIL,"Remove directory operation failed.");//550
		return ;
	}
	ftp_reply(sess,FTP_RMDIROK,"Remove directory operation successful.");
}

static void do_dele(session_t *sess)
{
	if(unlink(sess->arg)<0) //ɾ���ļ�
	{
		ftp_reply(sess,FTP_FILEFAIL,"Delete operation failed.");//550
		return ;
	}
	ftp_reply(sess,FTP_DELEOK,"Delete operation sucessfully.");
}

static void do_rest(session_t *sess)
{
	//����ϵ�������λ����Ϣ
	sess->restart_pos=str_to_longlong(sess->arg);
	char text[1024]={0};
	sprintf(text,"Restart position accepted (%lld)",sess->restart_pos);
	ftp_reply(sess,FTP_RESTOK,text);
}
static void do_size(session_t *sess)
{
	struct stat buf;
	if(stat(sess->arg,&buf)<0)
	{
		ftp_reply(sess,FTP_FILEFAIL,"SIZE operation failed.");//550
		return ;
	}
	//���������ͨ�ļ��Ļ�
	//���ж��Ƿ�����ͨ�ļ�
	if (!S_ISREG(buf.st_mode))
	{	//���������ͨ�ļ� ����Ӧ������Ϣ
		ftp_reply(sess,FTP_FILEFAIL,"Could not get file size.");//550
		return ;
	}
	else
	{
		char text[1024]={0};
		sprintf(text,"%lld",(long long)buf.st_size);//��ȡ��ͨ�ļ��Ĵ�С
		ftp_reply(sess,FTP_SIZEOK,text);
	}
}

static void do_rnfr(session_t *sess)
{
	sess->rnfr_name=(char *)malloc(strlen(sess->arg)+1);
	memset(sess->rnfr_name,0,strlen(sess->arg)+1);
	strcpy(sess->rnfr_name,sess->arg);
	ftp_reply(sess,FTP_RNFROK,"Ready for RNTO.");
}
static void do_rnto(session_t *sess)
{
	if(sess->rnfr_name==NULL)//֮ǰû���յ�RNFR����
	{
		ftp_reply(sess,FTP_NEEDRNFR,"RNFR required first.");
		return ;
	}
	rename(sess->rnfr_name,sess->arg);
	ftp_reply(sess,FTP_RENAMEOK,"Rename successful.");
	free(sess->rnfr_name);
	sess->rnfr_name=NULL;
}

static void do_retr(session_t *sess)
{
	//�����ļ�   �ϵ�����   �ͻ������Ȼᷢ��PASV or PORT����
	//������������
	if(get_transferfd(sess)==0)//������������ʧ��	
		return ;
	long long offset=sess->restart_pos;
	sess->restart_pos=0;
	//�ȴ�Ҫ���ص��ļ�
	int fd=open(sess->arg,O_RDONLY);
	if(fd==-1)
	{
		ftp_reply(sess,FTP_FILEFAIL,"Failed to open file.");
		return ;
	}
	//��Ҫ�򿪵��ļ��Ӷ���
	int ret=lock_file_read(fd);
	if(ret==-1)
	{
		ftp_reply(sess,FTP_FILEFAIL,"Lock file failed.");
		return ;
	}
	//�����豸�ļ����ܱ�����  ����Ҫ�ж��ļ��Ƿ�����ͨ�ļ�
	struct stat sbuf;
	ret=fstat(fd,&sbuf);
	if(!S_ISREG(sbuf.st_mode))//���������ͨ�ļ�
	{
		ftp_reply(sess,FTP_FILEFAIL,"The file is not a common file.");
		return ;
	}
	//lseek() ���������ļ�������fd�����Ĵ��ļ���ƫ�������¶�λ������ƫ����
	if(offset!=0)
	{
		ret=lseek(fd,offset,SEEK_SET);//SEEK_SET :ƫ��������Ϊƫ���ֽ�
		if(ret<0)
		{
			ftp_reply(sess,FTP_FILEFAIL,"DingWei file failed!");
			return ;
		}
	}
	//150
	char text[1024];
	if(sess->is_ascii)
	{
		sprintf(text,"Opening ASCII mode data connection for %s (%lld)",
			sess->arg,(long long)sbuf.st_size);
	}
	else
	{
		sprintf(text,"Opening BINARY mode data connection for %s (%lld)",
			sess->arg,(long long)sbuf.st_size);
	}
	ftp_reply(sess,FTP_DATACONN,text);//150

	int flag=0;
	//�����ļ�
	//����1   Ч�ʲ���   ԭ��: read write������������ϵͳ����
	//�����漰���û��ռ����ں˿ռ�ĵ����ݿ���   Ч�ʲ���
	/*
	int flag=0;//��־λ ��ʾ��Ӧ�ļ������
	char buf[4096]={0};
	while (1)
	{
		ret=read(fd,buf,sizeof(buf));
		if(ret==-1)
		{
			if(errno==EINTR)
				continue;
			else
			{
				flag=1;//��ʾ����
				break;
			}
		}
		else if(ret==0)//��ʾ��ȡ�����ļ�ĩβ
		{
			flag=0;   //
			break;
		}
	   //��ȡ����һ��������  д�������׽�����
		if (writen(sess->data_fd,buf,ret) != ret)
		{
			flag=2;
			break;
		}
	}
	*/
	//����2
	long long bytes_to_send=sbuf.st_size;
	if(offset >  bytes_to_send) //�ϵ��λ�ô��������ļ��Ĵ�С  ˵���д���
	{
		bytes_to_send=0;
	}
	else
		bytes_to_send-=offset;
	
	sess->bw_transfer_start_sec = get_time_sec();
	sess->bw_transfer_start_usec = get_time_usec();
	while(bytes_to_send)
	{
		int num_this_time=bytes_to_send > 4096 ? 4096 : bytes_to_send;

		ret=sendfile(sess->data_fd,fd,NULL,num_this_time);
		if(ret==-1)   //������errno==EINTR�����  ��Ϊsendfile()�����ں��н��п������ݵ�
		{
			flag=2;
			break;
		}
		limit_rate(sess,ret,0);
		bytes_to_send -= ret;
	}
	if (bytes_to_send==0)
	{
		flag=0;
	}
	//226��Ӧ��
	ftp_reply(sess,FTP_TRANSFEROK,"Directory send OK.");

	//�ر������׽���
	close(sess->data_fd);
	sess->data_fd=-1;
	close(fd);
	
	if(flag==0)
	{
		//226
		ftp_reply(sess,FTP_TRANSFEROK,"Transfer complete.");
	}
	else if(flag==1)
	{
		//426
		ftp_reply(sess,FTP_BADSENDNET,"Failure reading from local file.");
	}
	else if(flag==2)
	{
		//451
       ftp_reply(sess,FTP_BADSENDFILE,"Failure writing to network stream .");
	}

	start_cmdio_alarm();//���¿�����������ͨ������	ԭ���� ֮ǰ�������п��ܹر�
}

//�ϴ��ļ�   �ϵ�����   �ͻ������Ȼᷢ��PASV or PORT����
static void do_stor(session_t *sess)
{
	/*
	STOR  RETR
	REST  REST
	STOR  RETR
	APPE
	*/
	upload_common(sess,0);
}
/////////////////////////////////////////////////////////////////////
static void do_noop(session_t *sess) //�ú���  ���Է�ֹ���жϿ�
{
	ftp_reply(sess,FTP_NOOPOK,"NOOP ok.");
}

static void do_quit(session_t *sess)
{
	ftp_reply(sess,FTP_GOODBYE,"Goodbye.");
	exit(EXIT_SUCCESS);
}
/*
�ú����ǽ����������ݴ����ͨ�����Ͽ�  ������Ͽ���������ͨ��
*/
static void do_abor(session_t *sess)
{

}