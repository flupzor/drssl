#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "queue.h"

#define CIPHER_LIST "ALL"


#ifdef THREADED

#ifndef WIN32
#include <pthread.h>
#define THREAD_CC
#define THREAD_TYPE                    pthread_t
#define THREAD_CREATE(tid, entry, arg) pthread_create(&(tid), NULL, \
                                                      (entry), (arg))
#else
#include <windows.h>
#define THREAD_CC                      __cdecl
#define THREAD_TYPE                    DWORD
#define THREAD_CREATE(tid, entry, arg) do { _beginthread((entry), 0, (arg));\
                                            (tid) = GetCurrentThreadId();   \
                                       } while (0)
#endif

#endif //THREADED

/* Types */
typedef enum san_type_e {
    NONE,
    DNS,
    IP,
    EMAIL,
    UNKNOWN
} san_type;

struct subjectaltname {
    char     *value;
    san_type type;

    TAILQ_ENTRY(subjectaltnames) entries;
};

struct certinfo {
    X509            *cert;
    STACK_OF(X509)  *stack;
    char            *commonname;
    unsigned short   peer_uses_ca;
    unsigned short   found_ca;

    TAILQ_HEAD(, subjectaltnames) san_head;
};

struct sslconn {
    SSL_CTX *ctx;
    BIO *bio;
    int sock;
    SSL *ssl;
    unsigned short sslversion;
    char *host_ip;
    unsigned short port;
    unsigned short use_post_ssl_connection_checks;
    struct certinfo *certinfo;
};
/* Types */


void
global_init(void) {
    SSL_library_init();
    SSL_load_error_strings();

    RAND_load_file("/dev/urandom", 1024);
}

/* 0: Not a CA, 1: A CA */
int
x509IsCA(X509 *cert) {
   int purpose_id;

   purpose_id = X509_PURPOSE_get_by_sname("sslclient");

   /* final argument to X509_check_purpose() is whether to check for CAness */
   if (X509_check_purpose(cert, purpose_id + X509_PURPOSE_MIN, 1))
        return 1;
   else return 0;
}


/* Use: 2(SSLv2), 3(SSLv3), 10(TLS1.0), 11(TLS1.1), 12(TLS1.2) */
int
setup_client_ctx(struct sslconn *conn, unsigned short type) {
    if (!conn)
        return -1;

    switch (type) {
        case 2:
            conn->ctx = SSL_CTX_new(SSLv23_method());
            SSL_CTX_set_options(conn->ctx, SSL_OP_ALL|SSL_OP_NO_SSLv3);
            break;
        case 3:
            conn->ctx = SSL_CTX_new(SSLv3_client_method());
            SSL_CTX_set_options(conn->ctx, SSL_OP_ALL|SSL_OP_NO_SSLv2);
            break;
        case 10:
            conn->ctx = SSL_CTX_new(TLSv1_client_method());
            break;
#ifdef HAVE_TLSV1_1_CLIENT_METHOD
        case 11:
            conn->ctx = SSL_CTX_new(TLSv1_1_client_method());
            break;
#endif
#ifdef HAVE_TLSV1_2_CLIENT_METHOD
        case 12:
            conn->ctx = SSL_CTX_new(TLSv1_2_client_method());
            break;
#endif
        default:
            fprintf(stderr, "Wrong SSL version/type provided to %s()\n", __func__);
            return -2;
    }
    conn->sslversion = type;

    SSL_CTX_set_verify_depth(conn->ctx, 20);
    if (SSL_CTX_set_cipher_list(conn->ctx, CIPHER_LIST) != 1) {
        fprintf(stderr, "Error in setting cipher list, " \
                        "no valid ciphers provided in \"%s\"\n",
                        CIPHER_LIST);
        return -3;
    }

    return 0;
}


int
create_client_socket (int * client_socket,
                      const char * server,
                      int port,
                      int time_out_milliseconds) {
    struct addrinfo  hints;
    struct addrinfo *res;
    int              rc;
    int              mysock = -1;
    char             portstr[24];

    struct timeval  *wait_tv = NULL;
    struct timeval   preset_tv;
    unsigned int     preset_tvlen = sizeof preset_tv;


    memset(&hints, 0, sizeof(struct addrinfo));

    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family   = PF_UNSPEC;

    /* Get addrinfo */
    snprintf(portstr, 24, "%d", port);
    rc = getaddrinfo(server, &portstr[0], &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "Error: Failed to getaddrinfo (%s, %s, *, *)\n", server, portstr);
        return 1;
    }


    /* Create new socket */
    if ((mysock = socket(res->ai_family, res->ai_socktype, res->ai_protocol)) < 0) {
        fprintf(stderr, "Error: Failed to create socket\n");
        return 1;
    }


    /* Grab timeout setting */
    if (getsockopt (mysock, SOL_SOCKET, SO_RCVTIMEO, (char *)&preset_tv, &preset_tvlen) < 0) {
        fprintf(stderr, "Error: Failed to get the timeout setting\n");
        return 1;
    }


    /* Set connection timeout on the socket */
    wait_tv = (struct timeval *) malloc (sizeof (struct timeval));
    wait_tv->tv_sec = (time_out_milliseconds - (time_out_milliseconds % 1000)) / 1000;
    wait_tv->tv_usec = (time_out_milliseconds % 1000) * 1000;
    if (setsockopt (mysock, SOL_SOCKET, SO_RCVTIMEO, (char *)wait_tv, sizeof *wait_tv) < 0) {
        fprintf(stderr, "Error: Failed to set the timeout setting\n");
        return 1;
    }
    free (wait_tv);
    wait_tv = NULL;


    /* Connecting socket to host on port with timeout */
    if (connect(mysock, res -> ai_addr, res -> ai_addrlen) < 0) {
        fprintf(stderr, "Failed to connect\n");
        return 1;
    } else {
        /* Socket is succesfuly connected */
        setsockopt (mysock, SOL_SOCKET, SO_KEEPALIVE, 0, 0);

        *client_socket = mysock;
        return 0;
    }

    /* Failure */
    return 1;
}



/* Connect the struct sslconn object using a BIO */
int
connect_bio_to_serv_port(struct sslconn *conn) {
    int rc, count, sock;
    char *tmp;

    fprintf(stderr, "%s\n", __func__);

    if (!conn || !conn->host_ip)
        return -1;

    if (create_client_socket (&sock, conn->host_ip, conn->port, 30*1000) != 0) {
        fprintf(stderr, "Error: failed to connect to \"%s\" on port \'%d\'\n",
                        conn->host_ip, conn->port);
        return -2;
    }
    fprintf(stderr, "Connected to \"%s\" on port \'%d\'\n", conn->host_ip, conn->port);
    conn->sock = sock;
    return 0;
}

/* Connect struct sslconn object using SSL over an existing BIO */
int
connect_ssl_over_socket(struct sslconn *conn) {
    fprintf(stderr, "%s\n", __func__);

    if (!conn || !conn->host_ip || !conn->sock)
        return -1;

    conn->ssl = SSL_new(conn->ctx);
    if (!conn->ssl) {
        return -2;
    }


    /* Connecting the Socket to the SSL layer */
    conn->bio = BIO_new_socket (conn->sock, BIO_NOCLOSE);
    if (!conn->bio) {
        fprintf(stderr, "Error: Failed to tie the socket to a SSL BIO\n");
        SSL_free(conn->ssl);
        return -3;
    }
    fprintf(stderr, "BIO created from socket\n");

    SSL_set_bio(conn->ssl, conn->bio, conn->bio);
    if (SSL_connect(conn->ssl) <= 0) {
        fprintf(stderr, "Error connecting SSL\n");
        return -4;
    }
    return 0;
}

/* <0: error, 0: No SAN found, 1: SAN found */
int
extract_subjectaltnames(struct sslconn *conn) {
    int i, j, extcount;
    unsigned short found_san = 0;
    X509_EXTENSION          *ext;
    int                     NID_from_ext = NID_undef; /* Initialize with undefined NID (Numerical ID
                                                      of a type of ASN1 object) */
    unsigned char           *data;
    STACK_OF(CONF_VALUE)    *val;
    CONF_VALUE              *nval;
    X509V3_EXT_METHOD       *meth;
    void                    *ext_str = NULL;
    struct subjectaltname  *p_san;

    fprintf(stderr, "%s\n", __func__);

    if (!conn || !conn->certinfo || !conn->certinfo->cert)
        return -1;

    /* Compare the subjectAltName DNS value with the host value */
    if ((extcount = X509_get_ext_count(conn->certinfo->cert)) > 0) {
        /* Run through all the extensions */
        for (i = 0; i < extcount; i++) {
            ext = X509_get_ext(conn->certinfo->cert, i);
            NID_from_ext = OBJ_obj2nid(X509_EXTENSION_get_object(ext));

            /* Subject Alt Name? */
            if (NID_from_ext == NID_subject_alt_name) {
                found_san = 1;

                meth = X509V3_EXT_get(ext);
                if (!meth)
                    break;

                data = ext->value->data;

#if (OPENSSL_VERSION_NUMBER > 0x00907000L)
                if (meth->it)
                    ext_str = ASN1_item_d2i(NULL, &data, ext->value->length, ASN1_ITEM_ptr(meth->it));
                else
                    ext_str = meth->d2i(NULL, &data, ext->value->length);
#else
                ext_str = meth->d2i(NULL, &data, ext->value->length);
#endif
                val = meth->i2v(meth, ext_str, NULL);
                for (j = 0;  j < sk_CONF_VALUE_num(val);  j++) {
                    nval = sk_CONF_VALUE_value(val, j);
                    if (!nval) {
                        return -9;
                    }

                    /* Register the SAN */
                    p_san = malloc(sizeof(struct subjectaltname));
                    if (!p_san) {
                        fprintf(stderr, "Error: out of memory\n");
                        return -10;
                    }

                    if (!strcasecmp(nval->name, "DNS")) {
                        p_san->type = DNS;
                    } else if (!strcasecmp(nval->name, "iPAddress")) {
                        p_san->type = IP;
                    } else if (!strcasecmp(nval->name, "email")) {
                        p_san->type = EMAIL;
                    } else {
                        p_san->type = UNKNOWN;
                    }

                    p_san->value = strdup(nval->value);
                    if (!p_san->value) {
                        fprintf(stderr, "Error: out of memory\n");
                        return -11;
                    }

                    TAILQ_INSERT_TAIL(&(conn->certinfo->san_head), p_san, entries);
                }
            }
        }
    }

    if (!found_san)
        return 0;

    return 1;
}

int
extract_commonname(struct sslconn *conn) {
    X509_NAME *subj;
    int cnt;
    char *cn;

    fprintf(stderr, "%s\n", __func__);

    if (!conn || !conn->certinfo || !conn->certinfo->cert)
        return -1;

    subj = X509_get_subject_name(conn->certinfo->cert);
    if (!subj) {
        fprintf(stderr, "Error: could not extract the Subject DN\n");
        return -2;
    }

    cnt = X509_NAME_get_text_by_NID(subj, NID_commonName, NULL, 0);
    cn = malloc(cnt + 1);
    if (!cn) {
        fprintf(stderr, "Error: out of memory\n");
        return -3;
    }
    cnt = X509_NAME_get_text_by_NID(subj, NID_commonName, cn, cnt + 1);

    conn->certinfo->commonname = cn;
    return 0;
}

int
extract_peer_certinfo(struct sslconn *conn) {
    int depth, i;

    if (!conn || !conn->ssl)
        return -1;

    fprintf(stderr, "%s\n", __func__);

    conn->certinfo = calloc(sizeof(struct certinfo), 1);
    if (!conn->certinfo) {
        fprintf(stderr, "Error: Out of memory\n");
        return -2;
    }
    TAILQ_INIT(&(conn->certinfo->san_head));

    conn->certinfo->cert = SSL_get_peer_certificate(conn->ssl);
    if (!conn->certinfo->cert) {
        fprintf(stderr, "Error: No peer certificate found in SSL.\n");
        return -3;
    }

    /* On the client side, the peer_cert is included. On the server side it is
     * not. Assume client side for now */
    conn->certinfo->stack = SSL_get_peer_cert_chain(conn->ssl);
    if (!conn->certinfo->stack) {
        fprintf(stderr, "Error: No peer certificate stack found in SSL\n");
        return -4;
    }

    /* List and register the SubjectAltNames */
    if (extract_subjectaltnames(conn) < 0) {
        return -5;
    }

    /* Extract and register the Common Name */
    if (extract_commonname(conn) < 0) {
        return -6;
    }

    /* Check if peer cert is a CA */
    conn->certinfo->peer_uses_ca = x509IsCA(conn->certinfo->cert);

    /* Got Root CA / self-signed CA from the service */
    depth = sk_X509_num(conn->certinfo->stack);
    for (i = 0; i < depth; i++) {
        if (X509_NAME_cmp(X509_get_subject_name(sk_X509_value(conn->certinfo->stack, i)),
                          X509_get_issuer_name (sk_X509_value(conn->certinfo->stack, i))) == 0) {
            conn->certinfo->found_ca = 1;
        }
    }

    /* Carry on */
    return 0;
}


void
display_conn_info(struct sslconn *conn) {
    struct subjectaltname *p_san, *tmp_p_san;
    char                  *tmp;
    int                    i, depth;
    fprintf(stderr, ": Host/IP           : %s\n", conn->host_ip);
    fprintf(stderr, ": Port              : %d\n", conn->port);
    fprintf(stderr, ": Socket no         : %d\n", conn->sock);
    switch (conn->sslversion) {
        case  0: fprintf(stderr, ": SSL/TLS version   : NONE\n"); break;
        case  2: fprintf(stderr, ": SSL/TLS version   : SSLv2\n"); break;
        case  3: fprintf(stderr, ": SSL/TLS version   : SSLv3\n"); break;
        case 10: fprintf(stderr, ": SSL/TLS version   : TLS1.0\n"); break;
        case 11: fprintf(stderr, ": SSL/TLS version   : TLS1.1\n"); break;
        case 12: fprintf(stderr, ": SSL/TLS version   : TLS1.2\n"); break;
        default: fprintf(stderr, ": SSL/TLS version   : UNKNOWN\n"); break;
    }

    if (conn->certinfo) {
        fprintf(stderr, ": Certificate?      : %s\n", conn->certinfo->cert ? "Yes" : "No");
        fprintf(stderr, ": Stack?            : %s\n", conn->certinfo->stack ? "Yes" : "No");
        fprintf(stderr, ": Root CA in stack? : %s\n", conn->certinfo->found_ca ? "Yes" : "No");
        fprintf(stderr, ": Self-Signed peer? : %s\n", conn->certinfo->peer_uses_ca ? "Yes" : "No");
        fprintf(stderr, ": Peer Signed peer? : %s\n", conn->certinfo->peer_uses_ca ? "Yes" : "No");

        depth = sk_X509_num(conn->certinfo->stack);
        for (i = 0; i < depth; i++) {
            tmp = X509_NAME_oneline(X509_get_subject_name(sk_X509_value(conn->certinfo->stack, i)), NULL, 0);
            fprintf(stderr, ": Subject DN        : %2d%*s %s\n", i, i + 2, "-|", tmp);
            free(tmp);

            tmp = X509_NAME_oneline(X509_get_issuer_name (sk_X509_value(conn->certinfo->stack, i)), NULL, 0);
            fprintf(stderr, ": Issuer DN         : %2d%*s %s\n", i, i + 2, "-|", tmp);
            free(tmp);

            if (X509_NAME_cmp(X509_get_subject_name(sk_X509_value(conn->certinfo->stack, i)),
                              X509_get_issuer_name (sk_X509_value(conn->certinfo->stack, i))) == 0) {
                conn->certinfo->found_ca = 1;
            }
        }


        for (p_san = TAILQ_FIRST(&(conn->certinfo->san_head)); p_san != NULL; p_san = tmp_p_san) {
            fprintf(stderr, ": Subject Alt Name  : %s\n", p_san->value);
            tmp_p_san = TAILQ_NEXT(p_san, entries);
        }
        fprintf(stderr, ": Common name       : %s\n", conn->certinfo->commonname);

    } else {
        fprintf(stderr, ": No Certificate info from.\n");
    }

    return;
}


int
connect_to_serv_port (char *servername,
                      unsigned short servport,
                      unsigned short sslversion) {
    struct sslconn *conn;

    fprintf(stderr, "%s\n", __func__);

    conn = calloc(sizeof(struct sslconn), 1);
    if (conn == NULL)
        return -1;

    conn->host_ip = servername;
    conn->port    = servport;

    /* Create SSL context */
    if (setup_client_ctx(conn, sslversion) < 0) {
        return -2;
    }

    /* TCP/IP connect */
    if (connect_bio_to_serv_port(conn) < 0) {
        return -3;
    }

    /* SSL connect */
    if (connect_ssl_over_socket(conn) < 0) {
        return -4;
    }
    fprintf(stderr, "SSL Connection opened\n");

    /* Extract peer cert */
    if (extract_peer_certinfo(conn) < 0) {
        return -5;
    }

    /* Display / Show the information we gathered */
    display_conn_info(conn);


    fprintf(stderr, "SSL Shutting down.\n");
    SSL_shutdown(conn->ssl);
    fprintf(stderr, "SSL Connection closed\n");

    return 0;

fail:
    SSL_clear(conn->ssl);
    SSL_free(conn->ssl);
    SSL_CTX_free(conn->ctx);
    return 1;
}




int main(int argc, char *argv[])
{
    /* char *servername    = "192.16.199.166"; */
    /* char *servername    = "www.twitter.com"; */
    char *servername    = "sso.nikhef.nl";
    unsigned short port = 443;

    global_init();


/*
    servername = getArgVal ("server", argc, argv);
    tmpChk     = getArgVal ("port", argc, argv);
    if (tmpChk)
        port = (int)strtol(tmpChk, (char **)NULL, 10);

    printf ("Connecting to server %s on port %d\n", servername, port);
*/
    return connect_to_serv_port(servername, port, 10);
}
