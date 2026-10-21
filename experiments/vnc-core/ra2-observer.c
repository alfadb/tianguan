/* Isolated 64x48 RA2 fixture; opens only the supplied DISPLAY. */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
  alarm(35);
  Display *d=XOpenDisplay(NULL);
  if(!d) { fputs("cannot open fixture display\n",stderr); return 1; }
  int s=DefaultScreen(d);
  if(DisplayWidth(d,s)!=64 || DisplayHeight(d,s)!=48 || DefaultDepth(d,s)!=24) return 2;
  XSetWindowAttributes a={0};
  a.override_redirect=True; a.background_pixel=0x112233;
  a.event_mask=KeyPressMask|KeyReleaseMask|PointerMotionMask|ButtonPressMask|ButtonReleaseMask|ExposureMask;
  Window w=XCreateWindow(d,RootWindow(d,s),0,0,64,48,0,CopyFromParent,InputOutput,CopyFromParent,CWOverrideRedirect|CWBackPixel|CWEventMask,&a);
  XMapRaised(d,w); XSetInputFocus(d,w,RevertToParent,CurrentTime); XSync(d,False);
  XImage *image=XGetImage(d,w,32,24,1,1,AllPlanes,ZPixmap);
  if(!image) return 3;
  unsigned long pixel=XGetPixel(image,0,0);
  XDestroyImage(image);
  if(pixel!=0x112233) return 4;
  puts("READY 64x48 depth=24 RGB=112233"); fflush(stdout);
  int kp=0,kr=0,bp=0,br=0,motion=0;
  while(!(kp&&kr&&bp&&br&&motion)) {
    XEvent e; XNextEvent(d,&e);
    if(e.type==Expose) XClearWindow(d,w);
    if((e.type==KeyPress||e.type==KeyRelease)&&XLookupKeysym(&e.xkey,0)==XK_a) {
      if(e.type==KeyPress) kp=1; else if(kp) kr=1;
      puts(e.type==KeyPress?"a press":"a release");
    }
    if(e.type==MotionNotify&&e.xmotion.x==16&&e.xmotion.y==12) motion=1;
    if((e.type==ButtonPress||e.type==ButtonRelease)&&e.xbutton.button==1&&e.xbutton.x==16&&e.xbutton.y==12) {
      motion=1; if(e.type==ButtonPress) bp=1; else if(bp) br=1;
      puts(e.type==ButtonPress?"button1 press at 16,12":"button1 release at 16,12");
    }
    fflush(stdout);
  }
  puts("PASS a press/release; pointer 16,12; button1 press/release"); fflush(stdout);
  for(;;) pause();
}
