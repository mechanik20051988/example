/* For use need to implement functions in libev */
#include <pthread.h>
#include <stdio.h>
#include <assert.h>
#include <uv.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <ev.h>
#include "queue.h"

typedef struct ev_loop ev_loop_t;
struct elem {
	int id;
	STAILQ_ENTRY(elem) entry;
};
STAILQ_HEAD(elem_queue, elem);

static struct elem_queue ev_queue;
static struct elem_queue uv_queue;
static struct ev_async ev_async_;
static struct ev_async uv_async_;
static struct ev_async ev_async_stop_;
static ev_loop_t *ev_loop_;
static ev_loop_t *ev_uv_loop_;
static uv_loop_t *uv_loop_;
static pthread_mutex_t ev_queue_mtx;
static pthread_mutex_t uv_queue_mtx;

static _Atomic int pipew = -1;

static void
ev_async_stop_cb(ev_loop_t *loop, ev_async *ev, int revents)
{

}

static void
ev_async_cb(ev_loop_t *loop, ev_async *ev, int revents)
{
	pthread_mutex_lock(&uv_queue_mtx);
	struct elem *elem, *tmp;
	STAILQ_FOREACH_SAFE(elem, &uv_queue, entry, tmp) {
		fprintf(stderr, "ev_elem %d ", elem->id);
		free(elem);
	}
	fprintf(stderr, "\n");
	STAILQ_INIT(&uv_queue);
	pthread_mutex_unlock(&uv_queue_mtx);
}

static void
ev_uv_async_cb(ev_loop_t *loop, ev_async *ev, int revents)
{
	pthread_mutex_lock(&ev_queue_mtx);
	struct elem *elem, *tmp;
	STAILQ_FOREACH_SAFE(elem, &ev_queue, entry, tmp) {
		fprintf(stderr, "uv_elem %d ", elem->id);
		free(elem);
	}
	fprintf(stderr, "\n");
	STAILQ_INIT(&ev_queue);
	pthread_mutex_unlock(&ev_queue_mtx);
}

static void 
uv_enqueue_message(uv_timer_t* handle)
{
	static int cnt;
	struct elem *elem = (struct elem *)malloc(sizeof(struct elem));
	assert(elem != 0);
	elem->id = cnt++;
	pthread_mutex_lock(&uv_queue_mtx);
	STAILQ_INSERT_TAIL(&uv_queue, elem, entry);
	pthread_mutex_unlock(&uv_queue_mtx);
	if (ev_loop_)
		ev_async_send(ev_loop_, &ev_async_);
}

static void
ev_enqueue_message (struct ev_loop *loop, ev_timer *w, int revents)
{
	static int cnt;
	struct elem *elem = (struct elem *)malloc(sizeof(struct elem));
	assert(elem != 0);
	elem->id = cnt++;
	pthread_mutex_lock(&ev_queue_mtx);
	STAILQ_INSERT_TAIL(&ev_queue, elem, entry);
	pthread_mutex_unlock(&ev_queue_mtx);
	if (ev_uv_loop_)
		ev_async_send(ev_uv_loop_, &uv_async_);
}

static void
uv_read_pipe(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf) 
{
	if (nread < 0) {
		ev_break(ev_loop_, EVBREAK_ALL);
		ev_async_send(ev_loop_, &ev_async_stop_);
		uv_stop(uv_loop_);
	} else if (nread > 0) {
		do {
			ev_process_events(ev_uv_loop_, pipew, EV_READ);
		} while (!ev_prepare_extern_loop_wait(ev_uv_loop_));
	}
	if (buf->base)
		free(buf->base);
}

static void 
alloc_buffer_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
	buf->base = (char*)malloc(suggested_size);
	buf->len = suggested_size;
}

void *
ev_thread_func(void *arg) 
{
	ev_loop_ = ev_loop_new(EVFLAG_AUTO | EVFLAG_ALLOCFD);
	assert(ev_loop_);
	ev_timer timer;
	ev_timer_init(&timer, ev_enqueue_message, 0, 1);
	ev_timer_start(ev_loop_, &timer);
	ev_async_init(&ev_async_, ev_async_cb);
	ev_async_start(ev_loop_, &ev_async_);
	ev_async_init(&ev_async_stop_, ev_async_stop_cb);
	ev_async_start(ev_loop_, &ev_async_stop_);
	ev_run(ev_loop_, 0);
	uv_stop(uv_loop_);
	ev_async_stop(ev_loop_, &ev_async_stop_);
	ev_async_stop(ev_loop_, &ev_async_);
	ev_timer_stop(ev_loop_, &timer);
	return (void *)NULL;
}

void *
uv_thread_func(void *arg)
{
	uv_loop_ = uv_default_loop();
	ev_uv_loop_ = ev_loop_new(EVFLAG_AUTO | EVFLAG_ALLOCFD);
	assert(&ev_uv_loop_);
	pipew = ev_get_pipew(ev_uv_loop_);
	uv_pipe_t pipe;
	uv_timer_t timer;
	uv_timer_init(uv_loop_, &timer);
	uv_timer_start(&timer, uv_enqueue_message, 0, 1000);
	uv_pipe_init(uv_loop_, &pipe, 0);
	uv_pipe_open(&pipe, pipew);
	ev_async_init(&uv_async_, ev_uv_async_cb);
	ev_async_start(ev_uv_loop_, &uv_async_);
	uv_read_start((uv_stream_t*)&pipe, alloc_buffer_cb, uv_read_pipe);
	while(!ev_prepare_extern_loop_wait(ev_uv_loop_))
		ev_process_events(ev_uv_loop_, pipew, EV_READ);
	uv_run(uv_loop_, UV_RUN_DEFAULT);
        ev_break(ev_loop_, EVBREAK_ALL);
        uv_read_stop((uv_stream_t*)&pipe);
        uv_close((uv_handle_t*)&pipe, NULL);
        ev_async_stop(ev_uv_loop_, &uv_async_);
        uv_timer_stop(&timer);
	return (void *)NULL;
}

int main()
{
	int rc = EXIT_SUCCESS;
	pthread_t evt, uvt;
	STAILQ_INIT(&ev_queue);
	STAILQ_INIT(&uv_queue);

	if (pthread_mutex_init(&ev_queue_mtx, NULL) != 0) {
		fprintf(stderr, "Failed to init ev mtx\n");
		return EXIT_FAILURE;
	}

	if (pthread_mutex_init(&uv_queue_mtx, NULL) != 0) {
		fprintf(stderr, "Failed to init uv mtx\n");
		rc = EXIT_FAILURE;
		goto destroy_ev_mtx;
	}

	if (pthread_create(&evt, NULL, ev_thread_func, NULL) < 0) {
		fprintf(stderr, "Failed to start ev thread\n");
		rc = EXIT_FAILURE;
		goto destroy_uv_mtx;
	}

	if (pthread_create(&uvt, NULL, uv_thread_func, NULL) < 0) {
		fprintf(stderr, "Failed to start uv thread\n");
		rc = EXIT_FAILURE;
		goto wait_ev_thread;
	}

	pthread_join(uvt, NULL);
wait_ev_thread:
	pthread_join(evt, NULL);
destroy_uv_mtx:
	pthread_mutex_destroy(&uv_queue_mtx);
destroy_ev_mtx:
	pthread_mutex_destroy(&ev_queue_mtx);
	return rc;
}
