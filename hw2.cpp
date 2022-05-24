
#include <iostream>
#include <vector>
#include <utility>
#include <pthread.h>
#include <unistd.h>
#include <semaphore.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include "hw2_output.h"

using namespace std;

typedef struct properPrivate{
    int gid; // unique id 
    int si; // area i-dimension
    int sj; // area j-dimension
    int tg; // time to wait
    int ng; // number of areas to clean
    vector<pair<int,int>> areas; // top-left corners of assigned areas to clean, len(areas) = ng
} Private;

typedef struct commanderInput{
    vector<pair<int,string>>* orders;
    pthread_t* tids ;
} commanderInput;

typedef struct triplet{
    int ik;
    int jk;
    int ck;
} Triplet;

typedef struct smoker{
    int sid; // unique id
    int ts; // time to wait
    int ns; // number of cigarettes
    vector<Triplet> smokeAreas; // the cells smoker will smoke
} Smoker;


////////// GLOBAL VARIABLES

int **grid; // grid
int n,m; //  grid dimensions
sem_t** gridSem; // semaphore for each cell

pthread_cond_t*** cvGatherer; // availability of the cells, whether a cell includes a gatherer
pthread_mutex_t availableGathererLock; // lock for cvGatherer

pthread_mutex_t breakLock; // lock for cvBreak
pthread_cond_t cvBreak = PTHREAD_COND_INITIALIZER; // cv that is signaled when a break or stop order comes
int Break; // 1 if a break order executed
int Stop; // 1 if a stop order executed

pthread_mutex_t contLock; // lock for cvCont
pthread_cond_t cvCont = PTHREAD_COND_INITIALIZER; // cv is signaled when a continue order comes


pthread_cond_t*** cvSmoker; // availability of the cells, whether a cell includes a smoker
pthread_mutex_t availableSmokerLock; // lock for cvSmoker

pthread_cond_t*** cvLittering; // availability of the cell, whether a cell is in a littering area
pthread_mutex_t availableLitteringLock; // lock for cvLittering

int **gridLitter; // number of littering smokers at the same time
pthread_mutex_t leaveSmokeLock;

pthread_mutex_t smokeMutex;

///////////////// FUNCTIONS

int signal(sem_t* s){
    return sem_post(s);
}  

int wait(sem_t* s){
    return sem_wait(s);
} 

void printPrivate(Private* p){
    cerr << "GID: " << p->gid << endl;
    cerr << "si x sj: " << p->si << "x" << p->sj << endl;
    cerr << "t: " << p->tg << endl;
    cerr << "n: " << p->ng << endl;
    for(auto v : p->areas)
        cerr << "(" << v.first << "," << v.second << ")" << endl;
}

void printGrid(){
    for(int i=0;i<n;i++){ // cigbutt counts
        for(int j=0;j<m;j++){
            cerr << grid[i][j] << " ";
            if(grid[i][j]<10)
                cerr << " ";
        }
        cerr << endl;
    }
}
void signalAfterSmoke(int ik, int jk, int sid, bool destroy);

int waitCells(pair<int,int>& coord, int si, int sj, int gid){ // wait for gatherers' area, smokers' cells, smokers' littering area
    int boundary_i = coord.first+si < n ? coord.first+si : n ;
    int boundary_j = coord.second+sj < m ? coord.second+sj : m;
    bool checkAgain = true;

    // check cvGatherer: checking if all cells are free
    while(checkAgain){
        checkAgain = false;
        pthread_mutex_lock(&availableGathererLock);
        pthread_mutex_lock(&availableSmokerLock);
        pthread_mutex_lock(&availableLitteringLock);
        for(int i=coord.first;i<boundary_i;i++){
            for(int j=coord.second;j<boundary_j;j++){
                if(cvGatherer[i][j] == NULL && cvSmoker[i][j] == NULL && cvLittering[i][j] == NULL)
                    continue;
                if(cvGatherer[i][j] != NULL){
                    pthread_mutex_unlock(&availableSmokerLock);
                    pthread_mutex_unlock(&availableLitteringLock);
                    pthread_cond_wait(cvGatherer[i][j],&availableGathererLock);// unlocks the lock before blocking, locks after returning

                    pthread_mutex_lock(&breakLock);
                    if(Stop){
                        pthread_mutex_unlock(&breakLock);
                        pthread_mutex_unlock(&availableGathererLock);
                        //pthread_mutex_unlock(&availableSmokerLock);
                        //pthread_mutex_unlock(&availableLitteringLock);
                        hw2_notify(GATHERER_STOPPED,gid,0,0);
                        return 2;
                    }
                    else if(Break){
                        pthread_mutex_unlock(&breakLock);
                        pthread_mutex_unlock(&availableGathererLock);
                        //pthread_mutex_unlock(&availableSmokerLock);
                        //pthread_mutex_unlock(&availableLitteringLock);
                        
                        hw2_notify(GATHERER_TOOK_BREAK,gid,0,0);
                        pthread_mutex_lock(&contLock);
                        pthread_cond_wait(&cvCont,&contLock);
                        pthread_mutex_unlock(&contLock);
                        
                        pthread_mutex_lock(&breakLock);
                        if(Stop){ // may a stop order can come while we were on a break
                            pthread_mutex_unlock(&breakLock);
                            //pthread_mutex_unlock(&availableGathererLock);
                            //pthread_mutex_unlock(&availableSmokerLock);
                            //pthread_mutex_unlock(&availableLitteringLock);
                            hw2_notify(GATHERER_STOPPED,gid,0,0);
                            return 2;
                        }
                        pthread_mutex_unlock(&breakLock);
                        hw2_notify(GATHERER_CONTINUED, gid, 0 ,0);
                        return 1;
                    }
                    pthread_mutex_unlock(&breakLock);
                    pthread_mutex_unlock(&availableGathererLock);
                    checkAgain = true;
                    break;
                }
                if(cvSmoker[i][j] != NULL){
                    //cerr << "G" << gid << "is waiting on " << i << " - " << j  << " for a smoker" << endl;
                    pthread_mutex_unlock(&availableGathererLock);
                    pthread_mutex_unlock(&availableLitteringLock);
                    pthread_cond_wait(cvSmoker[i][j],&availableSmokerLock);
                    //cerr << "G" << gid << "is continuing on " << i << " - " << j  << " from a smoker" << endl;
                    pthread_mutex_lock(&breakLock);
                    if(Stop){
                        pthread_mutex_unlock(&breakLock);
                        //pthread_mutex_unlock(&availableGathererLock);
                        pthread_mutex_unlock(&availableSmokerLock);
                        //pthread_mutex_unlock(&availableLitteringLock);
                        hw2_notify(GATHERER_STOPPED,gid,0,0);
                        return 2;
                    }
                    else if(Break){
                        pthread_mutex_unlock(&breakLock);
                        //pthread_mutex_unlock(&availableGathererLock);
                        pthread_mutex_unlock(&availableSmokerLock);
                        //pthread_mutex_unlock(&availableLitteringLock);
                        
                        hw2_notify(GATHERER_TOOK_BREAK,gid,0,0);
                        pthread_mutex_lock(&contLock);
                        pthread_cond_wait(&cvCont,&contLock);
                        pthread_mutex_unlock(&contLock);
                        pthread_mutex_lock(&breakLock);
                        if(Stop){ // may a stop order can come while we were on a break
                            pthread_mutex_unlock(&breakLock);
                            pthread_mutex_unlock(&availableGathererLock);
                            //pthread_mutex_unlock(&availableSmokerLock);
                            //pthread_mutex_unlock(&availableLitteringLock);
                            hw2_notify(GATHERER_STOPPED,gid,0,0);
                            return 2;
                        }
                        pthread_mutex_unlock(&breakLock);
                        hw2_notify(GATHERER_CONTINUED, gid, 0 ,0);
                        return 1;
                    }
                    pthread_mutex_unlock(&breakLock);
                    pthread_mutex_unlock(&availableSmokerLock);
                    checkAgain = true;
                    break;
                }
                if(cvLittering[i][j]!=NULL){
                    //cerr << "G" << gid << "is waiting on " << i << " - " << j  << " for a littering cell" << endl;
                    pthread_mutex_unlock(&availableGathererLock);
                    pthread_mutex_unlock(&availableSmokerLock);
                    pthread_cond_wait(cvLittering[i][j],&availableLitteringLock);
                    //cerr << "G" << gid << " is continuing on " << i << " - " << j << " from a littering cell" << endl;
                    pthread_mutex_lock(&breakLock);
                    if(Stop){
                        pthread_mutex_unlock(&breakLock);
                        //pthread_mutex_unlock(&availableGathererLock);
                        //pthread_mutex_unlock(&availableSmokerLock);
                        pthread_mutex_unlock(&availableLitteringLock);
                        hw2_notify(GATHERER_STOPPED,gid,0,0);
                        return 2;
                    }
                    else if(Break){
                        pthread_mutex_unlock(&breakLock);
                        //pthread_mutex_unlock(&availableGathererLock);
                        //pthread_mutex_unlock(&availableSmokerLock);
                        pthread_mutex_unlock(&availableLitteringLock);
                        
                        hw2_notify(GATHERER_TOOK_BREAK,gid,0,0);
                        pthread_mutex_lock(&contLock);
                        pthread_cond_wait(&cvCont,&contLock);
                        pthread_mutex_unlock(&contLock);
                        pthread_mutex_lock(&breakLock);
                        if(Stop){ // may a stop order can come while we were on a break
                            pthread_mutex_unlock(&breakLock);
                            pthread_mutex_unlock(&availableGathererLock);
                            //pthread_mutex_unlock(&availableSmokerLock);
                            //pthread_mutex_unlock(&availableLitteringLock);
                            hw2_notify(GATHERER_STOPPED,gid,0,0);
                            return 2;
                        }
                        pthread_mutex_unlock(&breakLock);
                        hw2_notify(GATHERER_CONTINUED, gid, 0 ,0);
                        return 1;
                    }
                    pthread_mutex_unlock(&breakLock);
                    pthread_mutex_unlock(&availableLitteringLock);
                    checkAgain = true;
                    break;
                }
            }
            if(checkAgain) break;
        }
        pthread_mutex_unlock(&availableGathererLock);
        pthread_mutex_unlock(&availableSmokerLock);
        pthread_mutex_unlock(&availableLitteringLock);
    }
    for(int i=coord.first;i<boundary_i;i++){
        for(int j=coord.second;j<boundary_j;j++){
            wait(&gridSem[i][j]);
            pthread_mutex_lock(&availableGathererLock);
            cvGatherer[i][j] = new pthread_cond_t;
            pthread_cond_init(cvGatherer[i][j],NULL);
            pthread_mutex_unlock(&availableGathererLock);
        }
    }
    return 0;
}

bool waitForSmoke(int ik, int jk, int sid){ // wait for gatherers' area and smokers' cells

    bool checkAgain = true;
    int c = 0;
    while(checkAgain){
        checkAgain = false;
        pthread_mutex_lock(&availableGathererLock);
        pthread_mutex_lock(&availableSmokerLock);
        for(int i=ik-1;i<=ik+1;i++){
            for(int j=jk-1;j<=jk+1;j++){
                //cerr << "S" << sid << ": " << i << " - " << j <<": " << (cvGatherer[i][j] == NULL) << " && " << (cvSmoker[i][j] == NULL) << endl;
                if(cvGatherer[i][j] == NULL)
                    continue;
                if(cvGatherer[i][j] != NULL){ 
                    //cerr << "S" << sid << " is waiting for a gatherer at: " << i << " - " << j  << endl;
                    pthread_mutex_unlock(&availableSmokerLock);
                    pthread_cond_wait(cvGatherer[i][j],&availableGathererLock);

                    //cerr << "S" << sid << " is waiting from a gatherer at: " << i << " - " << j  << endl;
                    //cerr << "S" << sid << " going to smoke!! at " << i <<" - " << j<< endl;
                    // stop order
                    pthread_mutex_lock(&breakLock);
                    if(Stop){
                        pthread_mutex_unlock(&breakLock);
                        pthread_mutex_unlock(&availableGathererLock);
                        //pthread_mutex_unlock(&availableSmokerLock);
                        hw2_notify(SNEAKY_SMOKER_STOPPED,sid,0,0);
                        return false;
                    }
                    pthread_mutex_unlock(&breakLock);
                    pthread_mutex_unlock(&availableGathererLock);
                    checkAgain = true;
                    break;
                }
                
            }
            if(checkAgain)
                 break;
        }
        if(checkAgain) 
            continue;
        if(cvSmoker[ik][jk]!=NULL){
            //cerr << "S" << sid << " is waiting for a smoker at: " << i << " - " << j  << endl;
            pthread_mutex_unlock(&availableGathererLock);
            //cerr << "************************************" << endl;
            pthread_cond_wait(cvSmoker[ik][jk],&availableSmokerLock);
            //cerr << "S" << sid << "************************************" << endl;
            pthread_mutex_lock(&breakLock);
            if(Stop){
                pthread_mutex_unlock(&breakLock);
                pthread_mutex_unlock(&availableSmokerLock);
                //pthread_mutex_unlock(&availableGathererLock);
                hw2_notify(SNEAKY_SMOKER_STOPPED,sid,0,0);
                return false;
            }
            pthread_mutex_unlock(&breakLock);
            pthread_mutex_unlock(&availableSmokerLock);
            checkAgain = true;
            continue;
        }
        pthread_mutex_unlock(&availableSmokerLock);
        pthread_mutex_unlock(&availableGathererLock);
    }


    // all the area is okey, lock it
    pthread_mutex_lock(&availableLitteringLock);
    pthread_mutex_lock(&availableSmokerLock);
    for(int i = ik-1;i<=ik+1;i++){
        for(int j=jk-1;j<=jk+1;j++){
            if(i==ik && j==jk){
                //cerr << "S"<< sid <<   "waiting here" << endl;
                pthread_mutex_unlock(&availableSmokerLock);
                pthread_mutex_unlock(&availableLitteringLock);
                wait(&gridSem[i][j]);
                pthread_mutex_lock(&availableLitteringLock);
                pthread_mutex_lock(&availableSmokerLock);
                cvSmoker[i][j] = new pthread_cond_t;
                pthread_cond_init(cvSmoker[i][j],NULL);
                //pthread_mutex_unlock(&availableSmokerLock);
                continue;
            }
            pthread_mutex_lock(&leaveSmokeLock);
            if(++gridLitter[i][j] == 1){
                pthread_mutex_unlock(&leaveSmokeLock);
                cvLittering[i][j] = new pthread_cond_t;
                pthread_cond_init(cvLittering[i][j],NULL);
            }
            else
                pthread_mutex_unlock(&leaveSmokeLock);
        }
    }
    pthread_mutex_unlock(&availableSmokerLock);
    pthread_mutex_unlock(&availableLitteringLock);
    return true;
}

void signalCells(pair<int,int>& coord, int si, int sj,int gid){ 
    int boundary_i = coord.first+si < n ? coord.first+si : n ;
    int boundary_j = coord.second+sj < m ? coord.second+sj : m;
    pthread_mutex_lock(&availableGathererLock);
    for(int i=coord.first;i<boundary_i;i++){
        for(int j=coord.second;j<boundary_j;j++){
            pthread_cond_broadcast(cvGatherer[i][j]);
            //cerr << "G" << gid << " signaled " << i << "x" << j << endl;
            signal(&gridSem[i][j]);
            pthread_cond_destroy(cvGatherer[i][j]);
            cvGatherer[i][j] = NULL;
            
        }
    }
    pthread_mutex_unlock(&availableGathererLock);
}

void signalAfterSmoke(int ik, int jk, int sid, bool destroy){
    pthread_mutex_lock(&availableSmokerLock);
    //cerr << "S" << sid << " will signal" << endl;
    pthread_mutex_lock(&availableLitteringLock);
    for(int i= ik-1;i<=ik+1;i++){
        for(int j=jk-1;j<=jk+1;j++){
            if(i==ik && j==jk){
                //cerr << "S" << sid << " signaled littering cell " << i << " - " << j << endl;
                pthread_cond_broadcast(cvSmoker[i][j]);
                //pthread_mutex_unlock(&availableSmokerLock);
                if(!destroy) continue;
                pthread_cond_destroy(cvSmoker[i][j]);
                cvSmoker[i][j] = NULL;
                //pthread_mutex_unlock(&availableSmokerLock);
                signal(&gridSem[i][j]);
                continue;
            }
            //cerr << "S" << sid << " signaled littering cell " << i << " - " << j << endl;
            if(!destroy){
                pthread_cond_broadcast(cvLittering[i][j]);
                continue;
            }
            pthread_mutex_lock(&leaveSmokeLock);
            if(--gridLitter[i][j] == 0){
                pthread_mutex_unlock(&leaveSmokeLock);
                pthread_cond_broadcast(cvLittering[i][j]);
                pthread_cond_destroy(cvLittering[i][j]);
                cvLittering[i][j] = NULL;
            }
            else
                pthread_mutex_unlock(&leaveSmokeLock);
        }
    }
    pthread_mutex_unlock(&availableLitteringLock);
    pthread_mutex_unlock(&availableSmokerLock);
}

int cleanArea(pair<int,int>& coord, int si, int sj, int tg, int gid){
    int boundary_i = coord.first+si < n ? coord.first+si : n ;
    int boundary_j = coord.second+sj < m ? coord.second+sj : m;
    int tw;
    for(int i=coord.first;i<boundary_i;i++){
        for(int j=coord.second;j<boundary_j;j++){
            while(grid[i][j] > 0){
                struct timeval current_time;// sleeping time calculations
                struct timespec waiting_time;
                gettimeofday(&current_time, NULL);
                current_time.tv_usec += tg*1000;
                waiting_time.tv_sec = current_time.tv_sec + current_time.tv_usec/1000000;
                waiting_time.tv_nsec = (current_time.tv_usec%1000000)*1000;

                pthread_mutex_lock(&breakLock);
                tw = pthread_cond_timedwait(&cvBreak,&breakLock,&waiting_time); // it releases the breakLock before blocking, acquires it before returning
                if(tw == ETIMEDOUT)// not on a break  
                    pthread_mutex_unlock(&breakLock);
                else if(Stop){// end the thread
                    pthread_mutex_unlock(&breakLock);
                    signalCells(coord,si,sj,gid); //unlock all cells
                    hw2_notify(GATHERER_STOPPED,gid,0,0);
                    return 2;
                }
                else if(Break){// take a break
                    pthread_mutex_unlock(&breakLock);
                    signalCells(coord,si,sj,gid); //unlock all cells
                    
                    hw2_notify(GATHERER_TOOK_BREAK, gid, 0, 0);
                    pthread_mutex_lock(&contLock);
                    pthread_cond_wait(&cvCont,&contLock);
                    pthread_mutex_unlock(&contLock);
                    pthread_mutex_lock(&breakLock);
                    if(Stop){ // may a stop order can come while we were on a break
                        pthread_mutex_unlock(&breakLock);
                        pthread_mutex_unlock(&availableGathererLock);
                        //pthread_mutex_unlock(&availableSmokerLock);
                        //pthread_mutex_unlock(&availableLitteringLock);
                        hw2_notify(GATHERER_STOPPED,gid,0,0);
                        return 2;
                    }
                    pthread_mutex_unlock(&breakLock);
                    hw2_notify(GATHERER_CONTINUED, gid, 0 ,0);
                    return 1;
                }
                grid[i][j]--;
                hw2_notify(GATHERER_GATHERED, gid, i, j);
            }
        }
    }
    hw2_notify(GATHERER_CLEARED, gid, 0, 0);
    return 0;
}

bool smokeCigs(int ik, int jk,int ck, int ts, int sid){
    int tw;
    int i,j;
    int cig = 1;
    while(ck>0){
        switch(cig%8){
            case 1:
                i = ik-1;
                j = jk-1;
                break;
            case 2:
                i = ik-1;
                j = jk;
                break;
            case 3:
                i = ik-1;
                j = jk+1;
                break;
            case 4:
                i = ik;
                j = jk+1;
                break;
            case 5:
                i = ik+1;
                j = jk+1;
                break;
            case 6:
                i = ik+1;
                j = jk;
                break;
            case 7:
                i = ik+1;
                j = jk-1;
                break;
            case 0: // case 8:
                i = ik;
                j = jk-1;
                break;
        }
        struct timeval current_time;// sleeping time calculations
        struct timespec waiting_time;
        gettimeofday(&current_time, NULL);
        current_time.tv_usec += ts*1000;
        waiting_time.tv_sec = current_time.tv_sec + current_time.tv_usec/1000000;
        waiting_time.tv_nsec = (current_time.tv_usec%1000000)*1000;

        bool flag = true;
        while(flag){
            flag = false;
            pthread_mutex_lock(&breakLock);
            tw = pthread_cond_timedwait(&cvBreak,&breakLock,&waiting_time);
            if(tw==ETIMEDOUT){
                pthread_mutex_unlock(&breakLock);
            }
            else if(Stop){
                pthread_mutex_unlock(&breakLock);
                signalAfterSmoke(ik,jk,sid,true);
                pthread_cond_broadcast(&cvCont);
                hw2_notify(SNEAKY_SMOKER_STOPPED,sid,0,0);
                return false;
            }
            else if(Break){
                pthread_mutex_unlock(&breakLock);
                signalAfterSmoke(ik,jk,sid,false); // signal smoke and littering cells to let them gatherers to go break but dont destroy cvs
                flag = true;
            }
        }
        pthread_mutex_lock(&smokeMutex);
        grid[i][j]++;
        pthread_mutex_unlock(&smokeMutex);
        hw2_notify(SNEAKY_SMOKER_FLICKED,sid,i,j);
        ck--;
        cig++;
    }
    hw2_notify(SNEAKY_SMOKER_LEFT,sid,0,0);
    return true;
}

void executeOrders(vector<pair<int,string>>& orders, pthread_t* tids){
    int time = 0;
    int t;
    ///////////////////////////FOR PART2
    for(auto o: orders){
        t = o.first - time;
        usleep(t*1000);
        time = o.first;
        if(!o.second.compare("break")){ // send break signal to all gatherers
            hw2_notify(ORDER_BREAK,0,0,0);
            pthread_mutex_lock(&breakLock);
            Break = 1;
            pthread_mutex_unlock(&breakLock);
            pthread_cond_broadcast(&cvBreak);
        }
        else if(!o.second.compare("continue")){ // send continue signal to all gatherers
            hw2_notify(ORDER_CONTINUE,0,0,0);
            pthread_mutex_lock(&breakLock);
            if(Break){
                pthread_cond_broadcast(&cvCont);
            }
            Break = 0;
            pthread_mutex_unlock(&breakLock);
        }
        else if(!o.second.compare("stop")){ // send stop signal to all gatherers
            hw2_notify(ORDER_STOP,0,0,0);
            pthread_mutex_lock(&breakLock);
            if(Break){
                pthread_cond_broadcast(&cvCont);
            }
            pthread_cond_broadcast(&cvBreak);
            Break = 0;
            Stop = 1;
            pthread_mutex_unlock(&breakLock);
        }
        else
            cerr << "INVALID ORDER" << endl;
    }
}

////////// THREADS

void *gatherer(void *arg){ //arguments: grid, private
    Private* p = (Private*) arg;
    int si = p->si;
    int sj = p->sj;
    int tg = p->tg;
    int restartReq = 1;
    int backupCoord=0;
    int size = p->areas.size();
    int c = 0;
    while(restartReq){
        restartReq = 0; // should be unnecessary
        c++;
        for(int coord=backupCoord;coord<size;coord++ ){
            // WAIT FOR ALL CELLS IN THE AREA
            //cerr << "G" << p->gid << " waiting " << c <<  endl; 
            restartReq = waitCells(p->areas[coord], si, sj,p->gid);
            if(restartReq==1){
                backupCoord =coord;
                break;
            }
            else if(restartReq==2){
                pthread_exit(NULL);
            }

            hw2_notify(GATHERER_ARRIVED, p->gid, p->areas[coord].first, p->areas[coord].second);
            
            // CLEAN AREA
            restartReq = cleanArea(p->areas[coord], si, sj, tg, p->gid);
            if(restartReq==1){
                backupCoord =coord;
                break;
            }
            else if(restartReq==2){
                pthread_exit(NULL);
            }

            // SIGNAL FOR ALL CELLS IN THE AREA
            signalCells(p->areas[coord], si, sj, p->gid);
        }
    }
    
    hw2_notify(GATHERER_EXITED, p->gid, 0, 0);

    return NULL;
}


void *commander(void* arg){
    commanderInput* inp = (commanderInput*) arg;
    executeOrders(*(inp->orders),inp->tids);
    return NULL;
}

void *smoker(void *arg){
    Smoker* s = (Smoker*) arg;
    int sid = s->sid;
    int ns = s->ns;
    int ts = s->ts;
    vector<Triplet>& areas = s->smokeAreas;
    int size = areas.size();
    bool ok = true;

    for(int coord=0;coord<size;coord++){
        int ik = areas[coord].ik;
        int jk = areas[coord].jk;
        int ck = areas[coord].ck;

        // WAIT FOR THE CELL AND AREA
        ok = waitForSmoke(ik, jk, sid);
        if(!ok)
            pthread_exit(NULL);
        // hw_notify(stopped)

        hw2_notify(SNEAKY_SMOKER_ARRIVED, sid, ik, jk);

        // SMOKE
        ok = smokeCigs(ik, jk, ck, ts, sid);
        if(!ok)
            pthread_exit(NULL);
         // hw_notify(stopped)

        // SIGNAL THE CELL AND AREA
        signalAfterSmoke(ik, jk, sid,true);
    }
    
    hw2_notify(SNEAKY_SMOKER_EXITED, sid, 0, 0);

    return NULL;
}

void createGatherers(int numberOfPrivates, Private *privates, pthread_t *tids){
    for(int t=0;t<numberOfPrivates;t++){
        pthread_create(&tids[t],NULL,gatherer,(void*) &privates[t]); 
        hw2_notify(GATHERER_CREATED, privates[t].gid, 0, 0);
    }
}

int main(){
    
    // INITIALIZATION

    hw2_init_notifier();
    cin >> n >> m;
    grid = new int*[n];
    gridSem = new sem_t*[n];
    cvGatherer = new pthread_cond_t**[n];
    cvSmoker = new pthread_cond_t**[n];
    cvLittering = new pthread_cond_t**[n];
    gridLitter = new int*[n];
    for(int i=0;i<n;i++){ // Cigbutt counts
        grid[i] = new int[m];
        gridSem[i] = new sem_t[m];
        cvGatherer[i] = new pthread_cond_t*[m];
        cvSmoker[i] = new pthread_cond_t*[m];
        cvLittering[i] = new pthread_cond_t*[m];
        gridLitter[i] = new int[m];
        for(int j=0;j<m;j++){
            cin >> grid[i][j]; 
            sem_init(&gridSem[i][j],0,1); 
            cvGatherer[i][j] = NULL;
            cvSmoker[i][j] = NULL;
            cvLittering[i][j] = NULL;
            gridLitter[i][j] = 0;
        }
    }

    /////////// INPUT FOR PART-I
    
    int numberOfPrivates; // Number of proper privates
    cin >> numberOfPrivates;
    Private privates[numberOfPrivates]; // holds proper privates
    for(int i=0;i<numberOfPrivates;i++){ // Creation of proper privates
        cin >> privates[i].gid;
        cin >> privates[i].si;
        cin >> privates[i].sj;
        cin >> privates[i].tg;
        cin >> privates[i].ng;
        int x,y;
        for(int j=0;j<privates[i].ng;j++){
            cin >> x >> y;
            privates[i].areas.push_back(make_pair(x,y));
        }
    }

    

    
    // INPUT FOR PART II
    
    int numberOfOrders=0; // number of orders
    if(!cin.eof()){
        cin >> numberOfOrders;
    }
    vector<pair<int,string>> orders; // holds ms-order pairs

     

    for(int i=0;i<numberOfOrders;i++){ // taking orders
        int ms;
        string command;
        cin >> ms >> command;
        orders.push_back(make_pair(ms,command));
    }

    Break = 0;
    Stop = 0;
    //pthread_barrier_init(&barrier,NULL,numberOfPrivates+1);

    // INPUT FOR PART-III


    int numberOfSmokers = 0;
    if(!cin.eof())
        cin >> numberOfSmokers;

    Smoker smokers[numberOfSmokers];
    for(int i=0;i<numberOfSmokers; i++){
        cin >> smokers[i].sid;
        cin >> smokers[i].ts;
        cin >> smokers[i].ns;
        int ik,jk,ck;
        for(int j=0;j<smokers[i].ns;j++){
            Triplet triplet;
            cin >> triplet.ik >> triplet.jk >> triplet.ck;
            smokers[i].smokeAreas.push_back(triplet);
        }
    }
    
    pthread_t tids[numberOfPrivates];
    for(int t=0;t<numberOfPrivates;t++){
        pthread_create(&tids[t],NULL,gatherer,(void*) &privates[t]); 
        hw2_notify(GATHERER_CREATED, privates[t].gid, 0, 0);
    }
    
    // PART-II
    
    pthread_t ctid;
    commanderInput inp;
    inp.orders = &orders;
    inp.tids = tids;
    if(numberOfOrders) pthread_create(&ctid,NULL,commander,(void*) &inp); 

    pthread_t stids[numberOfSmokers];
    for(int s=0;s<numberOfSmokers;s++){
        pthread_create(&stids[s],NULL, smoker, (void*) &smokers[s]);
        hw2_notify(SNEAKY_SMOKER_CREATED, smokers[s].sid,0,0);
    }
    
    for(int t=0;t<numberOfPrivates;t++)
        pthread_join(tids[t],NULL);
    for(int s=0;s<numberOfSmokers;s++)
        pthread_join(stids[s],NULL);
    if(numberOfOrders) pthread_join(ctid,NULL);
    cerr << "--------------------GRID--------------------- " << endl;
    printGrid();

    cerr << "--------------------END-----------------------" << endl;
    return 0;
    
}