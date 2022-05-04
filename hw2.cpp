#include <iostream>
#include <vector>
#include <utility>
#include <pthread.h>
#include <unistd.h>
#include <semaphore.h>
#include <fcntl.h>
#include <signal.h>
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
int n,m; //  grid dimensions
pthread_mutex_t** gridMutex; // mutexes for each cell in grid
sem_t** gridSem; // semaphore for each cell


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

void waitCells(pair<int,int>& coord, int si, int sj){ 
    for(int i=coord.first;i<coord.first+si;i++){
        for(int j=coord.second;j<coord.second+sj;j++){
            //cerr << "waitinging on cell " << i << "-" << j  << " r: ";
            //cerr << wait(&gridCond[i][j], &gridMutex[i][j]) << endl;
            wait(&gridSem[i][j]);
            /*int fd_out = open("out", O_WRONLY | O_APPEND | O_CREAT, 0777);
            dup2(fd_out,1);

            cout << "WAITING ON (" << i << "," << j << ")" <<  endl; 

            cout << "PROCEEDING ON (" << i << "," << j << ")" <<  endl; */
        }
    }
}

// lock a dedicated grid area to gather cigbutts.
void lockCells(pair<int,int>& coord, int si, int sj){ 
    for(int i=coord.first;i<coord.first+si;i++){
        for(int j=coord.second;j<coord.second+sj;j++){
            pthread_mutex_lock(&gridMutex[i][j]);
        }
    }
}

// unlock the area to let others to run.
void unlockCells(pair<int,int>& coord, int si, int sj){
    for(int i=coord.first;i<coord.first+si;i++){
        for(int j=coord.second;j<coord.second+sj;j++){
            pthread_mutex_unlock(&gridMutex[i][j]);
        }
    }
}

void signalCells(pair<int,int>& coord, int si, int sj){ 
    for(int i=coord.first;i<coord.first+si;i++){
        for(int j=coord.second;j<coord.second+sj;j++){
            //signal(&gridCond[i][j]);
            signal(&gridSem[i][j]);
        }
    }
}


void cleanArea(pair<int,int>& coord, int si, int sj, int tg, int gid){
    int i,j;
    usleep(1000*tg);
    for(i=coord.first;i<coord.first+si;i++){
        for(j=coord.second;j<coord.second+sj;j++){
            while(grid[i][j] > 0){
                usleep(1000*tg);
                //cerr << "Gather one cigbutt from (" << i << "," << j << ")" << endl;
                grid[i][j]--;
                hw2_notify(GATHERER_GATHERED, gid, i, j);
            }
        }
    }
    hw2_notify(GATHERER_CLEARED, gid, 0, 0);
}

void executeOrders(vector<pair<int,string>>& orders, pthread_t* tids){
    int time = 0;
    int t;
    if(fork() == 0){ // child process to send signals ///////////////////////////FOR PART2
        for(auto o: orders){
            t = o.first - time;
            usleep(t*1000);
            time = o.first;
            if(!o.second.compare("break")){ // send break signal to all gatherers

                hw2_notify(ORDER_BREAK,0,0,0);
            }
            else if(!o.second.compare("continue")){ // send continue signal to all gatherers

                hw2_notify(ORDER_CONTINUE,0,0,0);
            }
            else if(!o.second.compare("stop")){ // send stop signal to all gatherers

                hw2_notify(ORDER_STOP,0,0,0);
            }
            else
                cerr << "INVALID ORDER" << endl;
        }
        return;
    }
    return;
}


////////// THREADS

void *gatherer(void *arg){ //arguments: grid, private
    Private* p = (Private*) arg;
    //cerr << "GID: " <<  p->gid << endl;
    int si = p->si;
    int sj = p->sj;
    int tg = p->tg;

    for(auto coord : p->areas){
        //  is semaphores and locks doing same job? probably yes but inspect it.
        //  nevertheless, it's working right.
        
        // WAIT FOR ALL CELLS IN THE AREA
        waitCells(coord, si, sj);

        // LOCK THE AREA
        //lockCells(coord, si, sj);

        hw2_notify(GATHERER_ARRIVED, p->gid, coord.first, coord.second);
        
        // CLEAN AREA
        cleanArea(coord, si, sj, tg, p->gid);

        // UNLOCK THE AREA
        //unlockCells(coord, si ,sj);

        // SIGNAL FOR ALL CELLS IN THE AREA
        signalCells(coord, si, sj);
    }

    hw2_notify(GATHERER_EXITED, p->gid, 0, 0);

    return NULL;
}


void *commander(void* arg){
    commanderInput* inp = (commanderInput*) arg;
    executeOrders(*(inp->orders),inp->tids);
}



int main(){
    /////////// PART-I

    /// input taking
    hw2_init_notifier();
    cin >> n >> m;
    grid = new int*[n];
    gridMutex = new pthread_mutex_t*[n];
    gridSem = new sem_t*[n];
    for(int i=0;i<n;i++){ // Cigbutt counts
        grid[i] = new int[m];
        gridMutex[i] = new pthread_mutex_t[m];
        gridSem[i] = new sem_t[m];
        for(int j=0;j<m;j++){
            cin >> grid[i][j];
            pthread_mutex_init(&gridMutex[i][j], NULL); 
            sem_init(&gridSem[i][j],0,1); 
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

    int numberOfOrders; // number of orders
    vector<pair<int,string>> orders; // holds ms-order pairs
    cin >> numberOfOrders; 

    for(int i=0;i<numberOfOrders;i++){ // taking orders
        int ms;
        string command;
        cin >> ms >> command;
        orders.push_back(make_pair(ms,command));
    }
    
    /// thread creating
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