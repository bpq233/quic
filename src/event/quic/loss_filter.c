#include <ngx_event_quic_connection.h>

int cnt1[100];

void init_Loss_Filter(Loss_Filter *loss_filter) {
    loss_filter->cnt = 0;
    loss_filter->rank = 0;
    loss_filter->loss_now = 0;
    for (int i = 0; i < 100l; i++) {
        loss_filter->loss[i]=0.1;
    }
}

void insertLoss(Loss_Filter *loss_filter, float loss) {
    loss_filter->loss_now = loss;
    loss_filter->loss[loss_filter->cnt]= loss;
    loss_filter->cnt++;
    loss_filter->cnt %= 100;
    updateRank(loss_filter, loss);
}

void updateRank(Loss_Filter *loss_filter, float loss){
    int c = 0;
    for (int i = 0; i < 100; i++) {
        if (loss_filter->loss[i] > loss - 1e-3) {
            c++;
        }
    }
    cnt1[100 - c]++;
    loss_filter->rank = c;
}