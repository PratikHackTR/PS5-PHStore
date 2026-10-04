#ifndef PH_PENDING_POLICY_H
#define PH_PENDING_POLICY_H
static int ph_pending_policy(int decrypted,int raw,int waiting_io){return decrypted>0||(raw>0&&!waiting_io);}
#endif
