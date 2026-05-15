#include <libssh2.h>
#include <libssh2_sftp.h>
#include <sys/socket.h>
#include <filesystem>
#include <netdb.h>
#include <arpa/inet.h>
#include <ncurses.h>
#include <algorithm>
#include <vector>
#include <string>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <cmath>
#include <chrono>
#include <cstdlib>

using namespace std;
namespace fs = filesystem;

struct file { 
    string name, path; 
    uint64_t size;
};

int sock, rows, cols, width, height, xOffset, aux, aSelected, bSelected, ch, split, virtualHeight, yOffset, tagOffset, savedFiles, totalFiles, pathCut;
uint64_t totalSize, savedSize, bps, secStamp;
WINDOW* win;
string sftpPath, localPath, host, user, keyfile;
vector<string> list;
vector<file> fileList;
LIBSSH2_SESSION* session;
LIBSSH2_SFTP* sftp;
bool logged, loading, confirmation, uploading, local;

uint64_t now_ms() { return chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now().time_since_epoch()).count(); }
vector<string> scan_dir(string s_path, bool s_local) {
    vector<string> output;
    
    if (s_local) {
        fs::directory_iterator end;

        for (fs::directory_iterator it(s_path); it!=end; it++) {
            const fs::directory_entry& entry = *it;
            string tag = string(entry.path().filename())+(entry.is_directory()?"/":"");
            output.push_back(tag);
        } return output;
    }

    string altPath = s_path[s_path.length()-1]=='/'?s_path.substr(0, s_path.length()-1):s_path;
    LIBSSH2_SFTP_HANDLE* dir = libssh2_sftp_opendir(sftp, altPath.c_str());
    if (!dir) { throw runtime_error("FAILED LISTING DIR: "+altPath); }

    char mem[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs;

    while (1) {
        int rc = libssh2_sftp_readdir(dir, mem, sizeof(mem), &attrs);
        if (rc<=0) { break; }
        string name(mem, rc);

        if (name=="." || name=="..") { continue; }
        if (LIBSSH2_SFTP_S_ISDIR(attrs.permissions)) { output.push_back(name+"/"); } 
        else { output.push_back(name); }
    } libssh2_sftp_closedir(dir);

    return output;
}
uint64_t get_file_size(string s_path, bool s_local) {
    if (s_local) { return fs::file_size(s_path); }
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    int error = libssh2_sftp_stat(sftp, s_path.c_str(), &attrs);
    if (error!=0) { return 0; }
    return attrs.filesize;
}
string crop_double(double number, int digits) {
    string output = to_string(number).substr(0, digits);
    if (output[digits-1]=='.') { output = output.substr(0, digits-1); }
    return output;
}
string size_to_string(uint64_t bytes, uint8_t precision, int8_t byteScale, bool binary) {
    byteScale = byteScale>6?6:byteScale;
    string unitDec[7] = {"B", "KB", "MB", "GB", "TB", "PB", "EB"};
    string unitBin[7] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    if (byteScale<0) {
        byteScale = 6;
        for (int i=0; i<5; i++) {
            if (bytes<pow(binary?1024:1000, (i+1))) { byteScale = i; break; }
        }
    } double relation = double(bytes)/(byteScale>0?pow(binary?1024:1000, byteScale):1);
    return crop_double(relation, precision)+(binary?unitBin[byteScale]:unitDec[byteScale]);
}
void create_socket(int port) {
    addrinfo hints{};
    addrinfo* res = nullptr;
    addrinfo* p = nullptr;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int error = getaddrinfo(host.c_str(), to_string(port).c_str(), &hints, &res);
    if (error!=0) { return; }

    sock = -1;
    for (p=res; p!=nullptr; p=p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock==-1) { continue; }
        if (connect(sock, p->ai_addr, p->ai_addrlen)==0) { break; }
        close(sock);
        sock = -1;
    } freeaddrinfo(res);
}
uint8_t set_variables() {
    string aux;
    bool checkbox[4] = {0, 0, 0, 0};
    const char* home = getenv("HOME");
    if (home==nullptr) { return 0; }
    ifstream conf(string(home)+"/.config/vault.conf");
    if (!conf.is_open()) { return 1; }
    
    while (conf>>aux) {
        if (aux=="HOST") { conf>>aux; host = aux; checkbox[0] = 1; }
        else if (aux=="USER") { conf>>aux; user = aux; checkbox[1] = 1; }
        else if (aux=="KEYFILE") { conf>>aux; keyfile = aux; checkbox[2] = 1; }
        else if (aux=="SFTP_PATH") { conf>>aux; sftpPath = aux; checkbox[3] = 1; }
    } for (int i=0; i<4; i++) { if (checkbox[i]==0) { return 2+i; } }
    return 6;
}
void recursive_save(string s_path, bool s_local) {
    string auxPath = s_local?sftpPath:localPath;
    vector<string> auxList = scan_dir(s_path, s_local);
    string subPath = s_path.substr(pathCut, s_path.length()-pathCut);
    if (s_local) { libssh2_sftp_mkdir(sftp, (sftpPath+subPath).c_str(), 0755); }
    else { fs::create_directories(localPath+subPath); }

    for (string name : auxList) {
        if (name[name.length()-1]=='/') { recursive_save(s_path+name, s_local); } 
        else {
            file newFile; 
            newFile.name = name;
            newFile.path = s_path+name;
            newFile.size = get_file_size(newFile.path, s_local);
            fileList.push_back(newFile);
        }
    } 
}
void recursive_delete(string s_path, bool s_local) {
    string auxPath = s_local?sftpPath:localPath;
    vector<string> auxList = scan_dir(s_path, s_local);
    string subPath = s_path.substr(pathCut, s_path.length()-pathCut);
    
    for (string name : auxList) {
        if (name[name.length()-1]=='/') { recursive_delete(s_path+name, s_local); }
        else if (local) { fs::remove((s_path+name).c_str()); }
        else { libssh2_sftp_unlink(sftp, (s_path+name).c_str()); }
    } if (local) { fs::remove((s_path.substr(0, s_path.length()-1)).c_str()); return; }
    libssh2_sftp_rmdir(sftp, (s_path.substr(0, s_path.length()-1)).c_str());
}
void show_login() {
    char buffer[32];

    win = newwin(height, width, 3, 2+xOffset);
    box(win, 0, 0);
    wrefresh(win);
    
    win = newwin(3, 28, rows/2-2, cols/2-13);
    box(win, 0, 0);
    mvwprintw(win, 1, 9, "PASSPHRASE");
    wrefresh(win);
    getnstr(buffer, 31);

    int wrong = libssh2_userauth_publickey_fromfile(session, user.c_str(), NULL, keyfile.c_str(), buffer);
    if (wrong) {  mvwprintw(win, 1, 9, "  FAILED  "); wrefresh(win); napms(500); return; } 
    else { mvwprintw(win, 1, 9, "CONNECTING"); wrefresh(win); napms(500); }
    
    sftp = libssh2_sftp_init(session);
    bSelected = 0;
    logged = 1;
    list = scan_dir(sftpPath, 0);
}
void show_loader() {
    if (fileList.size()==0) { loading = 0; return; }
    if (savedFiles==totalFiles) { loading = 0; return; }

    win = newwin(3, 27, rows/2-5, 4+xOffset);
    box(win, 0, 0);
    mvwprintw(win, 1, 11, "QUEUE");
    wrefresh(win);

    win = newwin(3, 19, rows/2-5, 31+xOffset);
    box(win, 0, 0);
    mvwprintw(win, 1, 6, "PROGRESS");
    wrefresh(win);

    win = newwin(7, 27, rows/2-3, 4+xOffset);
    box(win, 0, 0);
    int pos = 0;
    
    for (file listFile : fileList) {
        if (pos>4) { break; }
        string tag = listFile.name.substr(0, 19);
        mvwprintw(win, 1+pos, 4, tag.c_str());
        pos++;
    } wrefresh(win);

    win = newwin(7, 19, rows/2-3, 31+xOffset);
    box(win, 0, 0);
    wrefresh(win);

    mvwprintw(win, 1, 2, ("FILES "+to_string(savedFiles)+"/"+to_string(totalFiles)).c_str());
    mvwprintw(win, 2, 2, ("TOTAL "+size_to_string(totalSize, 4, -1, 1)).c_str());
    mvwprintw(win, 3, 2, ("SAVED "+size_to_string(savedSize, 4, -1, 1)).c_str());
    mvwprintw(win, 4, 2, ("PRCNT "+to_string(int(100.0f*(float)savedSize/(float)totalSize))+"%%").c_str());
    mvwprintw(win, 5, 2, ("SPEED "+size_to_string(bps, 4, -1, 1)+"/S").c_str());
    wrefresh(win);

    if (local) {
        FILE* localFile = fopen(fileList[0].path.c_str(), "rb");
        if (!localFile) { fileList.erase(fileList.begin()); return; }
        string subPath = fileList[0].path.substr(pathCut, fileList[0].path.length()-pathCut);
        LIBSSH2_SFTP_HANDLE* currentFile = libssh2_sftp_open(sftp, (sftpPath+subPath).c_str(), LIBSSH2_FXF_WRITE|LIBSSH2_FXF_CREAT|LIBSSH2_FXF_TRUNC, 0644);
        if (!currentFile) { fileList.erase(fileList.begin()); return; }

        char buffer[32768];
        bool failed = 1;

        while (true) {
            size_t n = fread(buffer, 1, sizeof(buffer), localFile);
            if (n>0) {
                char* ptr = buffer;
                savedSize += n; bps += n;  
                while (n>0) {
                    ssize_t nwritten = libssh2_sftp_write(currentFile, ptr, n);
                    if (nwritten<0) {
                        fclose(localFile);
                        libssh2_sftp_close(currentFile);
                    } ptr += nwritten;
                    n -= nwritten;
                } uint64_t now = now_ms();
                if (now-secStamp>=1000) { 
                    mvwprintw(win, 5, 8, (size_to_string(bps, 4, -1, 1)+"/S ").c_str());
                    bps = 0; secStamp = now; 
                } mvwprintw(win, 3, 8, (size_to_string(savedSize, 4, -1, 1)+" ").c_str());
                mvwprintw(win, 4, 8, (to_string(int(100.0f*(float)savedSize/(float)totalSize))+"%%").c_str());
                wrefresh(win);
            } if (feof(localFile)) { 
                failed = 0; 
                savedFiles++; 
                fileList.erase(fileList.begin());
                break; 
            } if (ferror(localFile)) { break; }
        } if (failed) { fileList.erase(fileList.begin()); return; }
        
        fclose(localFile);
        libssh2_sftp_close(currentFile);
        return;
    } 

    LIBSSH2_SFTP_HANDLE* currentFile = libssh2_sftp_open(sftp, (fileList[0].path).c_str(), LIBSSH2_FXF_READ, 0);
    if (!currentFile) { fileList.erase(fileList.begin()); return; }
    string subPath = fileList[0].path.substr(pathCut, fileList[0].path.length()-pathCut);
    FILE* localFile = fopen((localPath+subPath).c_str(), "wb");
    if (!localFile) { fileList.erase(fileList.begin()); return; }
    
    char buffer[32768];
    bool failed = 1;
    while (1) {
        ssize_t n = libssh2_sftp_read(currentFile, buffer, sizeof(buffer));
        if (n>0) { 
            savedSize += n; bps += n;  
            fwrite(buffer, 1, n, localFile); 
            uint64_t now = now_ms();
            if (now-secStamp>=1000) { 
                mvwprintw(win, 5, 8, (size_to_string(bps, 4, -1, 1)+"/S ").c_str());
                bps = 0; secStamp = now; 
            } mvwprintw(win, 3, 8, (size_to_string(savedSize, 4, -1, 1)+" ").c_str());
            mvwprintw(win, 4, 8, (to_string(int(100.0f*(float)savedSize/(float)totalSize))+"%%").c_str());
            wrefresh(win);
        } else if (n==0) { 
            failed = 0; 
            savedFiles++; 
            fileList.erase(fileList.begin());
            break; 
        } else { break; }
    } if (failed) { fileList.erase(fileList.begin()); return; }

    fclose(localFile);
    libssh2_sftp_close(currentFile);
}
void show_confirmation() {
    win = newwin(4, 18, rows/2-3, cols/2-8);
    box(win, 0, 0);
    mvwprintw(win, 1, 2, "[DLTE]");
    mvwprintw(win, 2, 2, "RETURN");
    mvwprintw(win, 1, 10, "[BACK]");
    mvwprintw(win, 2, 10, "BSPACE");
    wrefresh(win);
    ch = getch();
    if (ch==10) {
        string auxPath = local?localPath:sftpPath;
        for (int i=aSelected; i<=bSelected; i++) {
            if (list[i][list[i].length()-1]=='/') { recursive_delete(auxPath+list[i], local); }
            else if (local) { fs::remove(auxPath+list[i]); }
            else { libssh2_sftp_unlink(sftp, (auxPath+list[i]).c_str()); } 
        } list = scan_dir(local?localPath:sftpPath, local);
        if (bSelected>list.size()-1) { bSelected--; aSelected = bSelected; }
        confirmation = 0;
    } else if (ch==263) { confirmation = 0; return; }
}
void show_mdir() {
    char buffer[26];

    win = newwin(height, width, 3, 2+xOffset);
    box(win, 0, 0);
    wrefresh(win);
    
    win = newwin(3, 29, rows/2-3, cols/2-14);
    box(win, 0, 0);
    mvwprintw(win, 1, 8, "FOLDER NAME");
    wrefresh(win);

    win = newwin(3, 29, rows/2-1, cols/2-14);
    box(win, 0, 0);
    mvwprintw(win, 1, 1, " ");
    wrefresh(win);
    echo(); wgetnstr(win, buffer, 25); noecho();

    if (local) { fs::create_directories(localPath+string(buffer)); }
    else { libssh2_sftp_mkdir(sftp, (sftpPath+string(buffer)).c_str(), 0755); }
    list = scan_dir(local?localPath:sftpPath, local); 
}
int main() {
    secStamp = now_ms();
    logged = 0;
    loading = 0;
    uploading = 0;
    localPath = string(fs::current_path())+"/";
    uint8_t rc = set_variables();
    if (rc==0) { throw runtime_error("$HOME ENV VARIABLE NOT FOUND"); }
    else if (rc==1) { throw runtime_error("vault.conf NOT FOUND IN ~/.config"); }
    else if (rc==2) { throw runtime_error("HOST VARIABLE NOT FOUND IN vault.conf"); }
    else if (rc==3) { throw runtime_error("USER VARIABLE NOT FOUND IN vault.conf"); }
    else if (rc==4) { throw runtime_error("KEYFILE VARIABLE NOT FOUND IN vault.conf"); }
    else if (rc==5) { throw runtime_error("SFTP_PATH VARIABLE NOT FOUND IN vault.conf"); }
    create_socket(2222);
    libssh2_init(0);
    session = libssh2_session_init();
    libssh2_session_handshake(session, sock);
    
    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(0);

    getmaxyx(stdscr, rows, cols);

    if (cols<50) { throw runtime_error("TERMINAL WIDTH TOO SHORT"); };
    width = 50;
    height = rows-7;
    xOffset = cols-4>50?cols/2-26:0;
    yOffset = 0;
    tagOffset = 0;
    local = 0;

    while (1) {
        clear();
        refresh();
        
        if (!logged) { show_login(); continue; }
        
        string header = local?localPath:sftpPath;
        if (header.length()>width-4) { 
            int start = header.length()-width+4;
            header = header.substr(start, header.length()-start); 
        } win = newwin(3, width, 0, 2+xOffset);
        box(win, 0, 0);
        mvwprintw(win, 1, 2, header.c_str());
        wrefresh(win);
        
        win = newwin(4, width, rows-4, 2+xOffset);
        box(win, 0, 0);
        wrefresh(win);
        
        mvwprintw(win, 1, 2, "[EXIT]");
        mvwprintw(win, 2, 2, "CTRL+E");
        mvwprintw(win, 1, 10, "[SAVE]");
        mvwprintw(win, 2, 10, "CTRL+S");
        mvwprintw(win, 1, 18, "[FACE]");
        mvwprintw(win, 2, 18, "CTRL+F");
        mvwprintw(win, 1, 26, "[BACK]");
        mvwprintw(win, 2, 26, "BSPACE");
        mvwprintw(win, 1, 34, "[MDIR]");
        mvwprintw(win, 2, 34, "CTRL+D");
        mvwprintw(win, 1, 42, "[DLTE]");
        mvwprintw(win, 2, 42, "CTRL+X");
        wrefresh(win);
        
        win = newwin(height, width, 3, 2+xOffset);
        box(win, 0, 0);
        wrefresh(win);
        
        if (confirmation) { show_confirmation(); continue; }
        if (loading) { show_loader(); continue; }
        
        virtualHeight = height;
        split = list.size()/height;
        split = split>4?4:split;
        while (list.size()/(virtualHeight-2)>(float)split) { virtualHeight++; }

        if (bSelected%(virtualHeight-2)==0) { yOffset = 0; }
        else if (bSelected%(virtualHeight-2)==virtualHeight-3) { yOffset = virtualHeight-height; }
        else if (aSelected%(virtualHeight-2)==virtualHeight-3) { yOffset = virtualHeight-height; }

        for (int i=0; i<list.size(); i++) {
            string tag = list[i];
            if (i>=aSelected && i<=bSelected) { 
                tag = tag.substr(tagOffset, tag.length()-tagOffset);
                wattron(win, A_REVERSE); 
            } int len = (width-4)/(split+1)-1;
            tag = tag.substr(0, len);
            if (i>=virtualHeight-2) { 
                int y = 1+(i%(virtualHeight-2))-yOffset;
                if (y<=height-2 && y>0) { mvwprintw(win, y, 2+(len+1)*(i/(virtualHeight-2)), tag.c_str()); }
            } else { 
                int y = 1+i-yOffset;
                if (y<=height-2 && y>0) { mvwprintw(win, y, 2, tag.c_str()); }
            } if (i==bSelected) { wattroff(win, A_REVERSE); }
        } wrefresh(win);

        ch = getch();
        if (ch==5) { break; }
        if (ch==KEY_UP && bSelected>0) { 
            if (bSelected%(virtualHeight-2)>height-3) { yOffset--; }
            tagOffset = 0;
            bSelected--; aSelected = bSelected;
        } else if (ch==KEY_DOWN && bSelected<list.size()-1) {
            if (bSelected%(virtualHeight-2)>height-4) { yOffset++; }
            tagOffset = 0;
            bSelected++; aSelected = bSelected;
        } else if (ch==KEY_LEFT && bSelected>=virtualHeight-2 && split>0) {
            tagOffset = 0;
            bSelected -= virtualHeight-2;
            aSelected = bSelected;
        } else if (ch==KEY_RIGHT && bSelected<list.size()-virtualHeight+2 && split>0) {
            tagOffset = 0;
            bSelected += virtualHeight-2;
            aSelected = bSelected;
        } else if (ch==10) { 
            string name = list[bSelected];
            if (name[name.length()-1]=='/') { 
                if (local) { 
                    localPath += name;
                    list = scan_dir(localPath, 1); 
                } else { 
                    sftpPath += name;
                    list = scan_dir(sftpPath, 0); 
                }
            } tagOffset = 0;
            bSelected = 0; 
            aSelected = bSelected;
            yOffset = 0;
        } else if (ch==263 && ((local && localPath!="/") || (!local && sftpPath!="/vault/"))) {
            if (local) {
                string altPath = localPath[localPath.length()-1]=='/'?localPath.substr(0, localPath.length()-1):localPath;
                reverse(altPath.begin(), altPath.end());
                int index = altPath.length()+1-altPath.find('/')-1;
                localPath = localPath.substr(0, index);
                list = scan_dir(localPath, 1); 
            } else {
                string altPath = sftpPath[sftpPath.length()-1]=='/'?sftpPath.substr(0, sftpPath.length()-1):sftpPath;
                reverse(altPath.begin(), altPath.end());
                int index = altPath.length()+1-altPath.find('/')-1;
                sftpPath = sftpPath.substr(0, index);
                list = scan_dir(sftpPath, 0); 
            } tagOffset = 0;
            bSelected = 0;
            aSelected = bSelected;
            yOffset = 0;
        } else if (ch==32 && aSelected==bSelected) {
            tagOffset++;
            tagOffset = tagOffset>list[bSelected].length()-1?0:tagOffset;
        } else if (ch==337 && aSelected>0) { 
            if (aSelected%(virtualHeight-2)>height-3) { yOffset--; }
            tagOffset = 0;
            aSelected--; 
        } else if (ch==336 && bSelected<list.size()-1) { 
            if (bSelected%(virtualHeight-2)>height-4) { yOffset++; }
            tagOffset = 0;
            bSelected++; 
        } else if (ch==402 && bSelected<list.size()-virtualHeight+2) { 
            tagOffset = 0;
            bSelected += virtualHeight-2;
        } else if (ch==393 && aSelected>=virtualHeight-2) { 
            tagOffset = 0;
            aSelected -= virtualHeight-2;
        } else if (ch==6) { 
            local = !local; 
            tagOffset = 0;
            bSelected = 0;
            aSelected = bSelected;
            yOffset = 0;
            list = scan_dir(local?localPath:sftpPath, local); 
        } else if (ch==19) { 
            string auxPath = local?localPath:sftpPath;
            totalSize = 0;
            savedFiles = 0;
            savedSize = 0;
            pathCut = auxPath.length();
            fileList.clear();

            for (int i=aSelected; i<=bSelected; i++) { 
                if (list[i][list[i].length()-1]=='/') { recursive_save(auxPath+list[i], local); }
                else {
                    file newFile; 
                    newFile.name = list[i];
                    newFile.path = auxPath+list[i];
                    newFile.size = get_file_size(newFile.path, local);
                    fileList.push_back(newFile);
                }
            } for (file listFile : fileList) { totalSize += listFile.size; }
            totalFiles = fileList.size();
            loading = 1; 
        } else if (ch==24) { confirmation = 1; }
        else if (ch==4) { show_mdir(); }
    } 
    
    endwin();
    libssh2_sftp_shutdown(sftp);
    libssh2_session_disconnect(session, "QUITTING...");
    libssh2_session_free(session);
    close(sock);
    libssh2_exit();

    return 0;
}