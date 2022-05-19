
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


////////// GLOBAL VARIABLES

int **grid; // grid
int **numOfGatherers; // if there is a gatherer
int n,m; //  grid dimensions
pthread_mutex_t** gridMutex; // mutexes for each cell in grid
pthread_mutex_t checkMutex;
sem_t** gridSem; // semaphore for each cell
pthread_cond_t*** cvAvailable;
pthread_mutex_t availableLock;

pthread_mutex_t breakLock;
pthread_cond_t cvBreak = PTHREAD_COND_INITIALIZER;
int Break;
int Stop;

pthread_mutex_t contLock;
pthread_cond_t cvCont = PTHREAD_COND_INITIALIZER;

sem_t sleepReq;
sem_t awake;

pthread_mutex_t dummyLock;

///////// FUNCTIONS
/*
int signal(pthread_cond_t *cond){
    return pthread_cond_signal(cond);
}  
int wait(pthread_cond_t *cond, pthread_mutex_t *mutex){
    return pthread_cond_wait(cond,mutex);
} 
*/

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

int waitCells(pair<int,int>& coord, int si, int sj, int gid){ 
    int boundary_i = coord.first+si < n ? coord.first+si : n ;
    int boundary_j = coord.second+sj < m ? coord.second+sj : m;
    bool checkAgain = true;

    // check cvAvailable: checking if all cells are free
    while(checkAgain){
        checkAgain = false;
        pthread_mutex_lock(&availableLock);
        for(int i=coord.first;i<boundary_i;i++){
            for(int j=coord.second;j<boundary_j;j++){
                //if(gid == 1) cerr << "G1 is checking " << i << " - " << j << ": " << (cvAvailable[i][j] == NULL) << endl;
                if(cvAvailable[i][j] == NULL)
                    continue;
                //cerr << "G" << gid << " waiting on " << i << " - " << j << endl;
                pthread_cond_wait(cvAvailable[i][j],&availableLock);// unlocks the lock before blocking, locks after returning
                
                // stop and break orders should be added here
                pthread_mutex_lock(&breakLock);
                if(Stop){
                    pthread_mutex_unlock(&breakLock);
                    pthread_mutex_unlock(&availableLock);
                    hw2_notify(GATHERER_STOPPED,gid,0,0);
                    return 2;
                }
                else if(Break){
                    pthread_mutex_unlock(&breakLock);
                    pthread_mutex_unlock(&availableLock);
                    hw2_notify(GATHERER_TOOK_BREAK,gid,0,0);
                    pthread_mutex_lock(&contLock);
                    pthread_cond_wait(&cvCont,&contLock);
                    pthread_mutex_unlock(&contLock);
                    hw2_notify(GATHERER_CONTINUED, gid, 0 ,0);
                    return 1;
                }
                else
                    pthread_mutex_unlock(&breakLock);
                checkAgain = true;
                break;
            }
        }
        pthread_mutex_unlock(&availableLock);
    }

    //cerr << "G"<< gid << "is gonna lock the area" << endl; 
    
    for(int i=coord.first;i<boundary_i;i++){
        for(int j=coord.second;j<boundary_j;j++){
            wait(&gridSem[i][j]);
            pthread_mutex_lock(&availableLock);
            cvAvailable[i][j] = new pthread_cond_t;
            pthread_cond_init(cvAvailable[i][j],NULL);
            pthread_mutex_unlock(&availableLock);
        }
    }
    return 0;
}




void signalCells(pair<int,int>& coord, int si, int sj,int gid){ 
    int boundary_i = coord.first+si < n ? coord.first+si : n ;
    int boundary_j = coord.second+sj < m ? coord.second+sj : m;
    pthread_mutex_lock(&availableLock);
    for(int i=coord.first;i<boundary_i;i++){
        for(int j=coord.second;j<boundary_j;j++){
            pthread_cond_broadcast(cvAvailable[i][j]);
            //cerr << "G" << gid << " signaled " << i << "x" << j << endl;
            signal(&gridSem[i][j]);
            pthread_cond_destroy(cvAvailable[i][j]);
            cvAvailable[i][j] = NULL;
            
        }
    }
    pthread_mutex_unlock(&availableLock);
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
            Break = 0;
            Stop = 1;
            pthread_mutex_unlock(&breakLock);
            pthread_cond_broadcast(&cvBreak);
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
                pthread_mutex_lock(&dummyLock);
                //cerr << "G" << p->gid << " is waiting " <<  c << endl;
                pthread_mutex_unlock(&dummyLock);
            // WAIT FOR ALL CELLS IN THE AREA
            restartReq = waitCells(p->areas[coord], si, sj,p->gid);
                pthread_mutex_lock(&dummyLock);
                //cerr << "G" << p->gid << " is finished waiting " <<  c << endl;
                pthread_mutex_unlock(&dummyLock);
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
                                if(p->gid == 1 || p->gid==3)
                    usleep(300000);
                //pthread_mutex_lock(&dummyLock);
                //cerr << "G" << p->gid << " will be restarted " << c<< endl;
                //pthread_mutex_unlock(&dummyLock);                
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


int main(){
    /////////// PART-I

    /// input taking
    hw2_init_notifier();
    cin >> n >> m;
    grid = new int*[n];
    numOfGatherers = new int*[n];
    gridMutex = new pthread_mutex_t*[n];
    gridSem = new sem_t*[n];
    cvAvailable = new pthread_cond_t**[n];
    for(int i=0;i<n;i++){ // Cigbutt counts
        grid[i] = new int[m];
        numOfGatherers[i] = new int[m];
        gridMutex[i] = new pthread_mutex_t[m];
        gridSem[i] = new sem_t[m];
        cvAvailable[i] = new pthread_cond_t*[m];
        for(int j=0;j<m;j++){
            cin >> grid[i][j];
            numOfGatherers[i][j] = 0;
            pthread_mutex_init(&gridMutex[i][j], NULL); 
            sem_init(&gridSem[i][j],0,1); 
            cvAvailable[i][j] = NULL;
        }
    }
    
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
    
    int numberOfOrders; // number of orders
    vector<pair<int,string>> orders; // holds ms-order pairs
    cin >> numberOfOrders; 

    for(int i=0;i<numberOfOrders;i++){ // taking orders
        int ms;
        string command;
        cin >> ms >> command;
        orders.push_back(make_pair(ms,command));
    }

    Break = 0;
    Stop = 0;

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
    pthread_create(&ctid,NULL,commander,(void*) &inp); 
    
    for(int t=0;t<numberOfPrivates;t++)
        pthread_join(tids[t],NULL);
    pthread_join(ctid,NULL);
    cerr << "--------------------GRID--------------------- " << endl;
    printGrid();

    cerr << "--------------------END-----------------------" << endl;
    return 0;
    
}