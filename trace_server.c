#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <ccn/ccn.h>
#include <ccn/uri.h>
#include <ccn/keystore.h>
#include <ccn/signing.h>
#include <ccn/charbuf.h>
#include <ccn/reg_mgmt.h>
#include <ccn/ccn_private.h>
#include <ccn/ccnd.h>
#include <ccn/hashtb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>

#include "node_id.h"
#include "version.h"


//#define DEBUG

char node_id[128] = {0};

//for discarding duplicate interests
int prev_interest = 0;
int num_reply = 0;
int recv_reply = 0;
int processed_index = 0;


//data packet
struct data
{
    uint32_t num_message;
    uint32_t *message_length;
    char **fwd_message;       
};

int find_interest_name(const unsigned char *interest_msg,  struct ccn_parsed_interest *pi, const unsigned char **interest_name, int *interest_random_comp)
{
    //-----------------------------------------------------------------------//
    /// Arguments are interest message and parsed interest. Sets interest name
    /// and the interest random component for future usage
    //-----------------------------------------------------------------------//

    //get the full interest name
    int res;
    struct ccn_charbuf *name = ccn_charbuf_create();
    res = ccn_charbuf_append(name, interest_msg + pi->offset[CCN_PI_B_Name],
            pi->offset[CCN_PI_E_Name] - pi->offset[CCN_PI_B_Name]);
    struct ccn_charbuf *uri = ccn_charbuf_create();
    ccn_uri_append(uri, name->buf, name->length, 1);
    #ifdef DEBUG
        printf("uri = %s\n", ccn_charbuf_as_string(uri));
    #endif
    char *uri_string = malloc(strlen(ccn_charbuf_as_string(uri))+1);
    strcpy(uri_string, ccn_charbuf_as_string(uri));

    //remove the ccnx:/trace from the beginning of interest name
    uri_string = uri_string + strlen("ccnx:/trace");

    //get the last component, copy to int
    char *last_component = strrchr(uri_string, '/') + 1;
    #ifdef DEBUG
        printf("last component %s len %Zu\n", last_component, strlen(last_component));
    #endif
    sscanf((const char * )last_component, "%d", interest_random_comp);

    //get the remaining name, set it to interest name
    //uri - -len of ccnx:/trace - len of last component - 1 for the / + 1 for the \n
    int truncated_uri_length =  strlen(uri_string) - strlen(last_component) -  1 ;
    #ifdef DEBUG
        printf("uri length%d\n", truncated_uri_length);
    #endif
    *interest_name = malloc(sizeof(char) * truncated_uri_length + 1);
    if(interest_name == NULL)
    {   
        fprintf(stderr, "Can not allocate memory for interest_name\n");
        exit(1);
    }
    memset((void *)*interest_name, 0, truncated_uri_length+1);
    strncpy((char *)*interest_name, uri_string, truncated_uri_length);
    #ifdef DEBUG
        printf("Interest name %s\n", *interest_name);
    #endif
    return(0);
}


int get_faces(const unsigned char *interest_name, char **faces, int *num_faces, const unsigned char **longest_match)
{

    //-----------------------------------------------------------------------//
    
    ///Takes the interest name, fills in matching faces, number of faces
    ///and the longest match
    //-----------------------------------------------------------------------//

    #ifdef DEBUG
        printf("interest name%s\n", interest_name);
    #endif

    char *command = (char *) malloc(strlen((const char *)interest_name)+100);
    char readbuf[1024];
    int face_index=0;

    //make a duplicate of interest name
    char *search_str;
    if ((search_str = malloc (strlen((const char *)interest_name) + 1)) != NULL)
        strcpy (search_str, (const char*)interest_name);
    int len_search_str = strlen((const char *)search_str);

    //parse the ccndstatus for match
    while(len_search_str > 0)
    {
        sprintf(command, "%s%s%s", "ccndstatus|grep -w ", search_str, "|awk -F 'face:' '{print $2}' |awk '{print $1}'|sort|uniq");
        #ifdef DEBUG
            printf("%s\n", command); 
        #endif    

        //execute the command        
        FILE *fp = popen(command, "r");
        if (fp == NULL) 
        {
            printf("can not execute ccndstatus\n");
            exit(1);
        }

       //read buffer and get the first match
        while (fgets(readbuf, 1024, fp) != NULL)
        {
            readbuf[strlen(readbuf)-1] = '\0';
            faces[face_index] = malloc(strlen(readbuf)+1);
            memset(faces[face_index],0,strlen(readbuf)+1);
            strncpy(faces[face_index], readbuf, strlen(readbuf));
            face_index++;
        }

        //if faces are found, we are done, no need to match shorter prefixes, search_str is the prefix
        if(face_index > 0) 
        {
            *longest_match = malloc(sizeof(char) * strlen(search_str));
            if(longest_match== NULL)
            {
                fprintf(stderr, "Can not allocate memory for longest_match\n");
                exit(1);
            }
            strcpy((char *)*longest_match, search_str);
            #ifdef DEBUG
                printf("longest match %s strlen %Zu\n", *longest_match,  strlen((const char *)*longest_match));
            #endif
            free(search_str);
            break;    
        }

        //else, remove last component and retry
         #ifdef DEBUG
             printf("\nstring before%s\n", search_str);
         #endif

         char *last_component = strrchr(search_str, '/');
         if (last_component != NULL)
         {
             printf("last component %s len %Zu\n", last_component, strlen(last_component));
             *last_component = '\0';
         }
         printf("string after %s length  %Zu\n", search_str, strlen(search_str));
         len_search_str = strlen(search_str);
    }
    
    #ifdef DEBUG
        printf("number of faces %d\n", face_index);
    #endif
    *num_faces = face_index;
    free(command);
    return(0);
}

int find_remote_ip(char **face, int number_faces, char **return_ips, int *num_remote_ips)
{

    //-----------------------------------------------------------------------//
    ///Takes the list of faces for a prefix, finds the IP addresses from FIB 
    //-----------------------------------------------------------------------//

    int iter1 = 0;
    int return_ip_index = 0;
    char *command2 = (char *) malloc(1024);
    char fib_entry[1024];

    //for each face, find the matching ip address
    for (iter1 = 0; iter1 < number_faces; iter1++) 
    {
        memset(command2, 0, 1024);
        sprintf(command2, "%s%s%s", "ccndstatus |grep -w 'face: ", face[iter1], "'|awk -F 'remote:' '{print $2}' |awk -F ':' '{print $1}'|tr -s '\\n'|head -n 1");

        //execute command
        FILE *fp2 = popen(command2, "r");
        if (fp2 == NULL) 
        {
            printf("can not execute ccndstatus\n");
            exit(1);
        }

        //store the matching IPs
        while (fgets(fib_entry, 80, fp2) != NULL)
        {
            fib_entry[strlen(fib_entry)-1] = '\0';
            return_ips[return_ip_index] = malloc(strlen(fib_entry)+1);
            if(return_ips[return_ip_index]== NULL)
            {
                fprintf(stderr, "Can not allocate memory for storing remote IP\n");
                exit(1);
            }
            memset(return_ips[return_ip_index],0,(strlen(fib_entry)+1));
            strncpy(return_ips[return_ip_index], fib_entry, strlen(fib_entry));
            return_ip_index++;
            #ifdef DEBUG
                printf("storing ip address %s\n", fib_entry);               
            #endif
        }
    }

    //set the number of ips found
    //check readbuf length if we indeed found a route, not a blank line
    if (strlen(fib_entry) > 0)
        *num_remote_ips = return_ip_index;
    return(0);
}


char* swap_random(const unsigned char *interest_name, int interest_random_comp, char **new_interest_name, int *new_interest_random_comp)
{
    //-----------------------------------------------------------------------//
    ///Takes an interest name, swaps the random component for forwarding.
    ///the random seed is declared in the main
    //-----------------------------------------------------------------------//

    #ifdef DEBUG
        printf("%s %d\n", interest_name, interest_random_comp);
    #endif

    int rand_comp = rand();
    *new_interest_random_comp = rand_comp;

    //set the new interest name
    char *new_rand_comp = calloc(128, sizeof(int));
    sprintf(new_rand_comp, "%d", rand_comp);
    char *slash ="/";
    char *trace = "/trace";
    *new_interest_name = malloc(strlen(trace) + strlen((const char *)interest_name) + strlen(new_rand_comp) + 1);
    if(new_interest_name == NULL)
    {
        fprintf(stderr, "Can not allocate memory for new_interest_name\n");
        exit(1);
    }
    sprintf(*new_interest_name, "%s%s%s%s", trace, interest_name, slash, new_rand_comp);
    #ifdef DEBUG
        printf("Forwarding interest %s with random component %d\n\n\n", *new_interest_name, rand_comp);
    #endif

    //housekeeping
    free(new_rand_comp);
    return(0);
}

const unsigned char* manage_route(char *forwarding_interest_name, char *fwd_ip, int action)
{

    //-----------------------------------------------------------------------//
    /// Takes an interest name and remote IP. Adds or deletes a route based
    /// on action. Action 0 = add, 1 = deleter
    //-----------------------------------------------------------------------//

    //if we are adding route
    if (action == 0)
    {
        int add_route_length = strlen("ccndc add ") + strlen(forwarding_interest_name) + strlen(" tcp") +  strlen(fwd_ip) +1;
        char *add_route = malloc(add_route_length);
        if(add_route == NULL)
        {
            fprintf(stderr, "Can not allocate memory for add route command\n");
            exit(1);
        }
        sprintf(add_route, "%s%s%s%s", "ccndc add ", forwarding_interest_name, " tcp", fwd_ip);
        #ifdef DEBUG
            printf("adding route %s\n", add_route);
        #endif

        //execute the command
        FILE *fp = popen(add_route, "r");
        if (fp == NULL) 
        {
            printf("can not add route\n");
            exit(1);
        }
        pclose(fp);
        free(add_route);
    }

    //delete a route
    else if (action == 1)
    {
        int del_route_length = strlen("ccndc del ") + strlen(forwarding_interest_name) + strlen(" tcp") +  strlen(fwd_ip) +1;
        char *del_route = malloc(del_route_length);
        if(del_route == NULL)
        {
            fprintf(stderr, "Can not allocate memory for del route command\n");
            exit(1);
        }

        sprintf(del_route, "%s%s%s%s", "ccndc del ", forwarding_interest_name, " tcp", fwd_ip);
        #ifdef DEBUG
            printf("deleting route %s\n", del_route);
        #endif

        //execute the command
        FILE *fp = popen(del_route, "r");
        if (fp == NULL) 
        {
            printf("can not add route\n");
            exit(1);
        }
        pclose(fp);
        free(del_route);
    }
    return(0);
}



int construct_trace_response(struct ccn *h, struct ccn_charbuf *data, 
        const unsigned char *interest_msg, const struct ccn_parsed_interest *pi, unsigned char *mymsg, size_t size)
{


    //-----------------------------------------------------------------------//
    /// Constructs the trace response, signs them. data is sent by upcall
    //-----------------------------------------------------------------------//

    struct ccn_charbuf *name = ccn_charbuf_create();
    struct ccn_signing_params sp = CCN_SIGNING_PARAMS_INIT;
    int res;
    res = ccn_charbuf_append(name, interest_msg + pi->offset[CCN_PI_B_Name],
            pi->offset[CCN_PI_E_Name] - pi->offset[CCN_PI_B_Name]);
    if(res == -1)
    {
        fprintf(stderr, "Can not copy interest name to buffer\n");
        exit(1);
    }

    //sign the content, check if keystore exsists
    res = ccn_sign_content(h, data, name, &sp,  mymsg, size);
    if(res == -1)
    {
        fprintf(stderr, "Can not sign content\n");
        exit(1);
    }
    
    //free memory and return
    ccn_charbuf_destroy(&sp.template_ccnb);
    ccn_charbuf_destroy(&name);
    return res;
}


void *get_fwd_reply(struct ccn_charbuf *name_fwd, char *new_interest_name, char **fwd_reply, int *num_reply, int fwd_list_index, char *remote_ip)
{ 
    //-----------------------------------------------------------------------//
    /// forwards the interests and wait for reply. Timeout is hardcoded 8 secs.
    /// input is interest name to forward, sets the reply array (of strings)
    /// if a get times out, appends the remote ip with message and sets the
    /// appropriate string.
    //-----------------------------------------------------------------------//
    
    int res;
	const unsigned char *ptr;
    int i;

    struct data mymsg;

    res = ccn_name_from_uri(name_fwd, new_interest_name);
    if (res < 0) 
    {
        fprintf(stderr, "can not convert new interest name %s\n", new_interest_name);
        exit(1);
    }
    
    #ifdef DEBUG
        printf("expressing interest for %s\n", new_interest_name);
    #endif
    
    struct ccn_charbuf *ccnb = ccn_charbuf_create();
    if(ccnb == NULL)
    {
        fprintf(stderr, "Can not allocate memory for interest\n");
        exit(1);
    }
    res = ccn_name_from_uri(ccnb, (const char *)new_interest_name);
    if(res == -1)
    {
        fprintf(stderr, "Failed to assign name to interest");
        exit(1);
    }    

    //create the ccn handle
    struct ccn *ccn = ccn_create();
    if(ccn == NULL)
    {
        fprintf(stderr, "Can not create ccn handle\n");
        exit(1);
    }

    //connect to ccnd
    res = ccn_connect(ccn, NULL);
    if (res == -1) 
    {
        fprintf(stderr, "Could not connect to ccnd... exiting\n");
        exit(1);
    }

    #ifdef DEBUG
        printf("Connected to CCND, return code: %d\n", res);
    #endif
   
    //allocate buffer for response
    struct ccn_charbuf *resultbuf = ccn_charbuf_create();
    if(resultbuf == NULL)
    {
        fprintf(stderr, "Can not allocate memory for URI\n");
        exit(1);
    }

    //setting the parameters for ccn_get
    struct ccn_parsed_ContentObject pcobuf = { 0 };
    int timeout_ms = 8000;
    
    //express interest
    res = ccn_get(ccn, ccnb, NULL, timeout_ms, resultbuf, &pcobuf, NULL, 0);
    if (res == -1)
    {
        fprintf(stderr, "Did not receive answer for trace to %s\n", new_interest_name);

        //if we did not receive answer, set the answer
        fwd_reply[fwd_list_index] = malloc(sizeof (char *)* (strlen(remote_ip) + strlen("timeout")+1));
        if (fwd_reply[fwd_list_index] == NULL)
        {
            printf("Could not allocate memory for timeout reply message\n");
            exit(1);
        }
        sprintf(fwd_reply[fwd_list_index], "%s%s", remote_ip, ":TIMEOUT");
        *num_reply = 1;
    }

    else
    {
        //we received answer, parse it
        size_t length;
        ptr = resultbuf->buf;
        length = resultbuf->length;
        ccn_content_get_value(ptr, length, &pcobuf, &ptr, &length);

        //check if received some data
        if(length == 0)
        {    
            fprintf(stderr, "Received empty answer for trace to %s\n", new_interest_name);
            #ifdef DEBUG
                fprintf(stderr, "Received empty answer for trace to URI: %s\n", new_interest_name);
            #endif
        }

        memcpy(&mymsg.num_message, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);

        mymsg.message_length = malloc(sizeof(uint32_t)*mymsg.num_message);
        if (mymsg.message_length == NULL)
        {
            printf("Could not allocate memory for storing fwd reply message length\n");
            exit(1);
        }
        for(i=0; i < mymsg.num_message; i++)
        {
            memcpy(&mymsg.message_length[i], ptr, sizeof(uint32_t));
            ptr += sizeof(uint32_t);
        }

        //copy the replies to data packet
        mymsg.fwd_message = malloc(sizeof(char *) * mymsg.num_message);
        if (mymsg.fwd_message == NULL)
        {
            printf("Could not allocate memory for fwd reply message number\n");
            exit(1);
        }
        for(i=0; i < mymsg.num_message; i++)
        {
            mymsg.fwd_message[i] = malloc (sizeof(char) * mymsg.message_length[i]);
            if (mymsg.fwd_message[i] == NULL)
            {
                printf("Could not allocate memory for fwd reply message data\n");
                exit(1);
            }
            strncpy(mymsg.fwd_message[i], (const char *)ptr, mymsg.message_length[i]);
            ptr += mymsg.message_length[i];
            printf("%s\n", mymsg.fwd_message[i]);
        }

        //set the replies
        for(i=0; i < mymsg.num_message; i++)
        {
           fwd_reply[fwd_list_index + i] = malloc(sizeof (char *)*mymsg.message_length[i]);
            if (fwd_reply[fwd_list_index + i] == NULL)
            {
                printf("Could not allocate memory for reply\n");
                exit(1);
            }
           sprintf(fwd_reply[fwd_list_index + i], "%s", mymsg.fwd_message[i]);
        }
        *num_reply = mymsg.num_message;

    }
    //we are done here
    res = ccn_disconnect(ccn);
    ccn_charbuf_destroy(&resultbuf);
    ccn_charbuf_destroy(&ccnb);
    return(0);
}




enum ccn_upcall_res incoming_interest(struct ccn_closure *selfp,
        enum ccn_upcall_kind kind, struct ccn_upcall_info *info)
{


    //-----------------------------------------------------------------------//
    /// callback function, all interest matching ccnx:/trace will come here, 
    /// handle them as appropriate
    //-----------------------------------------------------------------------//

    struct ccn_charbuf *data = ccn_charbuf_create();
    int res = 0;
    char *faces[100]; //char* of faces
    char *remote_ips[100]; //char *remote ips
    int num_remote_ips = 0;
    int number_faces = 0;
    int i=0;
    int remote_ip_index = 0;

    // get the interest name length, last component
    const unsigned char *interest_name = NULL;
    int interest_random_comp = 0;
    const unsigned char *longest_prefix = NULL;
    char *new_interest_name = NULL;

    //data structures for forwarding interests
    struct ccn_charbuf *name_fwd = ccn_charbuf_create();
    char *fwd_reply[100];
    int fwd_list_index = 0;
    int fwd_message_length = 0;
    int num_reply=0;

    struct data return_data;

    int new_interest_random_comp = 0;
    int new_interest_random_comp_length = 0;
    char *new_interest_random_comp_str = NULL;

    unsigned char *buffer = NULL;
    unsigned char *reset_buffer = NULL;
    int iter = 0;
    size_t buffer_len = 0;

    int interest_random_comp_length = 0;
    char *interest_random_comp_string = NULL;

    int processed[10000]; //duplicate removal
    int flag  = 0;

    //switch on type of event
    switch (kind) 
    {
    case CCN_UPCALL_FINAL:
        return CCN_UPCALL_RESULT_OK;    
        break;

    case CCN_UPCALL_CONTENT:  
        break;
    
    case CCN_UPCALL_INTEREST:

        //received matching interest
        //get the interest name and random component from incoming packet   
        res = find_interest_name(info->interest_ccnb, info->pi, &interest_name, &interest_random_comp);
        #ifdef DEBUG
            printf("Interest name %s, random is %d\n", interest_name, interest_random_comp);
        #endif
        interest_random_comp_string = malloc(sizeof(char) * interest_random_comp_length + 1);
        if(interest_random_comp_string == NULL)
        {
            fprintf(stderr, "Can not allocate memory for interest_random_comp_string\n");
            exit(1);
        }
        sprintf(interest_random_comp_string, "%d", interest_random_comp);

        //check for duplicate messages
        for (iter = 0; iter < processed_index; iter++)
        {
            #ifdef DEBUG
                printf("processed value = %d\n",processed[iter]);
            #endif
            if (processed[iter] == interest_random_comp)
            {
                #ifdef DEBUG
                    printf("Duplicate\n");
                #endif
                flag = 1;
                break;
            }
        }

        //if duplicate message, do nothing. Otherwise, add to processed list and proceed
        if (flag == 1)
            break;
        processed[processed_index] = interest_random_comp;
        processed_index += 1;
        #ifdef DEBUG
            printf("processed index %d\n", processed_index);
        #endif

        //get the matching faces for this interest
        res = get_faces(interest_name, faces, &number_faces, &longest_prefix);
        #ifdef DEBUG
            for (i=0; i <number_faces; i++)
            {
                printf("face %s:  %s\n", faces[i], longest_prefix);
            }
        #endif

        //there is no such face, there is no route
        if (number_faces == 0)
        {
            
            return_data.num_message = 1;
            return_data.message_length =  malloc(sizeof(uint32_t) * 1);
            if(return_data.message_length == NULL)
            {
                fprintf(stderr, "Can not allocate memory for reply message, field 1\n");
                exit(1);
            }
            //replay appropriately
            return_data.message_length[0] = strlen(node_id)+1 + strlen(":NO ROUTE FOUND") ;
            return_data.fwd_message = malloc(sizeof(char *) * 1);
            return_data.fwd_message[0] = malloc(strlen(node_id)+1 + strlen(":NO ROUTE FOUND"));
            if(return_data.fwd_message == NULL|| return_data.fwd_message[0] == NULL)
            {
                fprintf(stderr, "Can not allocate memory for reply message, data\n");
                exit(1);
            }
            sprintf(return_data.fwd_message[0], "%s%s",  node_id, ":NO ROUTE FOUND");
        }

        //we have some faces, find if they are remote or local
        else
        {
            //get the number of remote ips
            res = find_remote_ip(faces, number_faces, remote_ips, &num_remote_ips);
            #ifdef DEBUG
                printf("%d longest_prefix %s\n", num_remote_ips, longest_prefix);
            #endif

            //if no remote ip found, this is local
            if (num_remote_ips == 0)
            {
                //does the name matches with longest prefix? otherwise, no such contetn
                if (strcmp((const char *)interest_name, (const char *)longest_prefix) == 0)
                {   
                    #ifdef DEBUG
                        printf("This is local\n");
                    #endif
                    //reply appropriately
                    return_data.num_message = 1;
                    return_data.message_length =  malloc(sizeof(uint32_t) * 1);
                    return_data.message_length[0] = strlen(node_id)+1 + strlen(":LOCAL") ;
                    return_data.fwd_message = malloc(sizeof(char *) * 1);
                    return_data.fwd_message[0] = malloc(strlen(node_id)+1 + strlen(":LOCAL"));
                    sprintf(return_data.fwd_message[0], "%s%s",  node_id, ":LOCAL");
                }
                
                //else, no such content
                else
                {
                    return_data.num_message = 1;
                    return_data.message_length =  malloc(sizeof(uint32_t) * 1);
                    if(return_data.message_length == NULL)
                    {
                        fprintf(stderr, "Can not allocate memory for reply message, field 1\n");
                        exit(1);
                    }
                    //reply appropriately
                    return_data.message_length[0] = strlen(node_id)+1 + strlen(":NO SUCH CONTENT") ;
                    return_data.fwd_message = malloc(sizeof(char *) * 1);
                    return_data.fwd_message[0] = malloc(strlen(node_id)+1 + strlen(":NO SUCH CONTENT"));
                    if(return_data.fwd_message == NULL|| return_data.fwd_message[0] == NULL)
                    {
                        fprintf(stderr, "Can not allocate memory for reply message, data\n");
                        exit(1);
                    }
                    sprintf(return_data.fwd_message[0], "%s%s",  node_id, ":NO SUCH CONTENT");
                }
            }

            //we found some remote ips for this face
            else
            {
                #ifdef DEBUG
                    printf("adding route and forwarding, expecting %d replies\n", num_remote_ips);
                #endif
                num_reply = num_remote_ips;

                //for each remote ip, swap random number and forward interest
                for (remote_ip_index = 0; remote_ip_index<num_remote_ips; remote_ip_index++)
                {
                    //swap the random string
                    swap_random(interest_name, interest_random_comp, &new_interest_name, &new_interest_random_comp);
                    new_interest_random_comp_str = malloc(sizeof(char) * new_interest_random_comp_length);
                    sprintf(new_interest_random_comp_str, "%d", new_interest_random_comp);
                    #ifdef DEBUG
                        printf("new interest name %s\n", new_interest_name);
                    #endif
  
                    //add to the processed list
                    processed[processed_index] = new_interest_random_comp;
                    processed_index += 1;

                    //add the remote route
                    manage_route(new_interest_name, remote_ips[remote_ip_index], 0);

                    //create fwd interest
                    res = ccn_name_from_uri(name_fwd, new_interest_name);
	                if (res < 0) 
                    {
		                fprintf(stderr, "can not convert new interest name %s\n", new_interest_name);
		                exit(1);
	                }

                    //express interest
                    get_fwd_reply(name_fwd, new_interest_name, &*fwd_reply, &num_reply, fwd_list_index, remote_ips[remote_ip_index]);
                    fwd_list_index += num_reply;
                    
                    //delete the route
                    manage_route(new_interest_name, remote_ips[remote_ip_index], 1);

                    //we are done with the new interest name, go back for the next remote ip
                    free(new_interest_name);

                }
                #ifdef DEBUG
                    printf("\n\n");
                    for (i = 0; i < fwd_list_index; i++)
                    {
                        printf("Reply is %s \n", fwd_reply[i]);
                    }
                    printf("\n\n");
                #endif

                //process and store the replies in a data packet
                return_data.num_message = fwd_list_index;
                return_data.message_length =  malloc(return_data.num_message);
                if(return_data.message_length == NULL)
                {
                    fprintf(stderr, "Can not allocate memory for reply message leangth\n");
                    exit(1);
                }

                //store the messages
                return_data.fwd_message = malloc(sizeof(char *) * return_data.num_message);
                for (i = 0; i < fwd_list_index; i++)
                {
                   
                    return_data.message_length[i] = strlen(node_id)+1 + strlen(":FWD ")+ strlen(fwd_reply[i])  ;
                    return_data.fwd_message[i] = malloc(return_data.message_length[i]);
                    if(return_data.fwd_message[i] == NULL)
                    {
                        fprintf(stderr, "Can not allocate memory for reply message number %d\n", i);
                        exit(1);
                    }
                    sprintf(return_data.fwd_message[i], "%s%s%s",  node_id, ":FWD ", fwd_reply[i]);
                    #ifdef DEBUG
                        printf("%s\n", return_data.fwd_message[i]);
                    #endif
                }
            }
        }
        

        //now we have the messages, pack them and send them back
        #ifdef DEBUG
            printf("return_data.num_message = %d\n", return_data.num_message);
        #endif

        for (iter = 0; iter<return_data.num_message; iter++)
        {
             #ifdef DEBUG
                 printf("message length = %d\n", return_data.message_length[iter]);
                 printf("message = %s\n", return_data.fwd_message[iter]);
             #endif            
             fwd_message_length += return_data.message_length[iter];
        }
       

        //pack the buffer for sending
        buffer = malloc(sizeof(uint32_t)* (1+ return_data.num_message) + fwd_message_length);
        if(buffer == NULL)
        {
            fprintf(stderr, "Can not allocate memory for return buffer %d\n", i);
            exit(1);
        }
        
        //we have to reset the pointer before sending
        reset_buffer = buffer;

        //copy num_fwd_interest
        memcpy(buffer, &return_data.num_message, sizeof(int));
   
        buffer += sizeof(uint32_t);
        buffer_len += 1*sizeof(uint32_t);
        //copy the lengths
        for (iter = 0; iter<return_data.num_message; iter++)
        {
            memcpy(buffer, &return_data.message_length[iter], sizeof(uint32_t));
            buffer += sizeof(uint32_t);
            buffer_len += sizeof(uint32_t);
        }

        //copy the strings
        for (iter = 0; iter<return_data.num_message; iter++)
        {
            memcpy(buffer, return_data.fwd_message[iter], return_data.message_length[iter]);
            buffer += return_data.message_length[iter];
            buffer_len += return_data.message_length[iter];
            #ifdef DEBUG
                printf("buffer len%Zu\n", buffer_len);
            #endif
        }

        //reset pointer
        buffer = reset_buffer;

        //send data packet
        construct_trace_response(info->h, data, info->interest_ccnb, info->pi, buffer, buffer_len);
        res = ccn_put(info->h, data->buf, data->length);
        printf("\n");

        //free all the allocate memory
        free((void*)interest_name);
        free((void *)longest_prefix);
        free(buffer);
        free(new_interest_random_comp_str);
        free(interest_random_comp_string);
        return CCN_UPCALL_FINAL;
        break;

	case CCN_UPCALL_INTEREST_TIMED_OUT:
		printf("request timed out - retrying\n");
		return CCN_UPCALL_RESULT_REEXPRESS;

	case CCN_UPCALL_CONTENT_UNVERIFIED: 
        printf("Could not verify content");
        return CCN_UPCALL_RESULT_ERR;

	case CCN_UPCALL_CONTENT_BAD:   
		printf("Bad content\n");
		return CCN_UPCALL_RESULT_ERR;

	default:
		printf("Unexpected response\n");
		return CCN_UPCALL_RESULT_ERR;
    }
    return CCN_UPCALL_RESULT_OK;
}

void usage(void)
{
    ///prints the usage and exits
    printf("%s version %s \n", SRV_PROGRAM, SRV_VERSION);
    printf("%s \n\n", SRV_PROGRAM);

    printf("  -h             print this help and exit\n");
    printf("  -V             print version and exit\n\n");
    exit(0);
}



int main(int argc, char **argv)
{

    //no argument necessary
    if(argc != 1)
    {
        usage();
        exit(1);
    }

    //seed the random
    srand ((unsigned int)time (NULL)*getpid());

    //get the node_id, IP address for now
    if (get_ip_addresses(node_id) == NULL)
    {
        fprintf(stderr, "Can not get node_id\n");
        exit(1);
    }
    //print node id
    printf("Node ID:%s\n", node_id);

    //create ccn handle
    struct ccn *ccn = NULL;
    
    //connect to CCN
    ccn = ccn_create();
    if (ccn_connect(ccn, NULL) == -1) 
    {
        fprintf(stderr, "Could not connect to ccnd");
        exit(1);
    }

    //create prefix we are interested in, register in FIB
    int res;
    struct ccn_charbuf *prefix = ccn_charbuf_create();

    //We are interested in anythin starting with /trace
    res = ccn_name_from_uri(prefix, "/trace");
    if (res < 0) 
    {
        fprintf(stderr, "Can not convert name to URI\n");
        exit(1);
    }

    //handle for upcalls, receive notifications of incoming interests and content.
    //specify where the reply will go
    struct ccn_closure in_interest = {.p = &incoming_interest};
    in_interest.data = &prefix;

    //set the interest filter for prefix we created
    res = ccn_set_interest_filter(ccn, prefix, &in_interest); 
    if (res < 0) 
    {
        fprintf(stderr, "Failed to register interest (res == %d)\n", res);
        exit(1);
    }

    //listen infinitely
    res = ccn_run(ccn, -1);
    printf("res run%d\n", res);

    //cleanup
    ccn_destroy(&ccn);
    ccn_charbuf_destroy(&prefix);
    exit(0);
}