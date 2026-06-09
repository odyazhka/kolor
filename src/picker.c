/*
 * picker.c — HSV color picker, pure Xlib
 * Экспортирует: int run_picker(const char *init_hex, char *out_hex)
 * Возвращает 1 если OK, 0 если Cancel
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

/* ── Layout ────────────────────────────────────────────────────────────────── */
#define WIN_W        420
#define WIN_H        460

#define PAD          14

#define SWATCH_X     PAD
#define SWATCH_Y     PAD
#define SWATCH_W     64
#define SWATCH_H     28
#define HEX_X        (SWATCH_X + SWATCH_W + 10)
#define HEX_Y        SWATCH_Y
#define HEX_W        110
#define HEX_H        28

#define SV_X         PAD
#define SV_Y         (SWATCH_Y + SWATCH_H + 12)
#define SV_W         340
#define SV_H         340

#define HUE_X        (SV_X + SV_W + 10)
#define HUE_Y        SV_Y
#define HUE_W        24
#define HUE_H        SV_H

#define BTN_W        90
#define BTN_H        28
#define BTN_Y        (SV_Y + SV_H + 12)
#define BTN_CANCEL_X (WIN_W - PAD - BTN_W*2 - 8)
#define BTN_OK_X     (WIN_W - PAD - BTN_W)

/* ── Colors ────────────────────────────────────────────────────────────────── */
#define C_BG         0x363636u
#define C_BORDER     0x505050u
#define C_INPUT_BG   0x1e1e1eu
#define C_TEXT       0xdcdcdcu
#define C_BTN_CANCEL 0x4b2d2du
#define C_BTN_OK     0x2d4b2du

/* ── HSV/RGB ───────────────────────────────────────────────────────────────── */
typedef struct { float r, g, b; } RGB;
typedef struct { float h, s, v; } HSV;

static RGB hsv2rgb(HSV c) {
    if (c.s == 0.f) { RGB o={c.v,c.v,c.v}; return o; }
    float hh = c.h / 60.f;
    int   i  = (int)hh;
    float f  = hh - floorf(hh);
    float p  = c.v*(1-c.s);
    float q  = c.v*(1-c.s*f);
    float t  = c.v*(1-c.s*(1-f));
    switch(i%6){
        case 0:{ RGB o={c.v,t,p}; return o; }
        case 1:{ RGB o={q,c.v,p}; return o; }
        case 2:{ RGB o={p,c.v,t}; return o; }
        case 3:{ RGB o={p,q,c.v}; return o; }
        case 4:{ RGB o={t,p,c.v}; return o; }
        default:{ RGB o={c.v,p,q}; return o; }
    }
}

static HSV rgb2hsv(RGB c) {
    float max = fmaxf(fmaxf(c.r,c.g),c.b);
    float min = fminf(fminf(c.r,c.g),c.b);
    float d   = max - min;
    HSV o; o.v = max; o.s = max==0?0:d/max;
    if(d==0){o.h=0;return o;}
    if     (max==c.r) o.h=60.f*fmodf((c.g-c.b)/d,6.f);
    else if(max==c.g) o.h=60.f*((c.b-c.r)/d+2.f);
    else              o.h=60.f*((c.r-c.g)/d+4.f);
    if(o.h<0) o.h+=360.f;
    return o;
}

/* ── X11 state ─────────────────────────────────────────────────────────────── */
static Display     *dpy;
static Window       win;
static GC           gc;
static Colormap     cmap;
static XFontStruct *font;
static Pixmap       buf;
static int          scr;
static Visual      *vis;
static int          depth;
static int          bytes_per_pixel;

/* XImage буферы для градиентов (рисуем в RAM, потом один XPutImage) */
static XImage      *sv_img  = NULL;
static XImage      *hue_img = NULL;
static int          sv_dirty  = 1;
static int          hue_dirty = 1;
static float        sv_hue_cached = -1.f; /* при какой hue пересчитан sv_img */

static HSV  hsv;
static char hex_buf[8];
static int  hex_cursor;
static int  hex_focused;

/* ── Pixel helpers ─────────────────────────────────────────────────────────── */

/* Конвертируем r,g,b [0..1] в pixel для текущего visual */
static unsigned long make_pixel(float r, float g, float b) {
    unsigned long ri = (unsigned long)(r * 255 + .5f);
    unsigned long gi = (unsigned long)(g * 255 + .5f);
    unsigned long bi = (unsigned long)(b * 255 + .5f);
    /* Для TrueColor visual (почти все современные иксы) */
    return (ri << 16) | (gi << 8) | bi;
}

static unsigned long pixel_from_hex(unsigned long h) {
    return make_pixel(((h>>16)&0xff)/255.f,
                      ((h>>8) &0xff)/255.f,
                      ( h     &0xff)/255.f);
}

/* Записываем пиксель в XImage (поддержка 24/32 bpp) */
static void img_put(XImage *img, int x, int y, unsigned long px) {
    if(bytes_per_pixel == 4) {
        unsigned int *p = (unsigned int*)(img->data + y*img->bytes_per_line + x*4);
        *p = (unsigned int)px;
    } else { /* 3 bpp — редко, но бывает */
        unsigned char *p = (unsigned char*)(img->data + y*img->bytes_per_line + x*3);
        p[0] = px & 0xff;
        p[1] = (px>>8) & 0xff;
        p[2] = (px>>16) & 0xff;
    }
}

/* ── XImage создание ───────────────────────────────────────────────────────── */
static XImage *make_image(int w, int h) {
    char *data = (char*)malloc((size_t)(w * h * bytes_per_pixel));
    return XCreateImage(dpy, vis, (unsigned)depth, ZPixmap, 0,
                        data, (unsigned)w, (unsigned)h, 32, 0);
}

/* ── Gradients в RAM ───────────────────────────────────────────────────────── */
static void build_sv_image(void) {
    if(!sv_img) sv_img = make_image(SV_W, SV_H);
    HSV honly = {hsv.h, 1, 1};
    RGB hrgb  = hsv2rgb(honly);
    for(int ix=0; ix<SV_W; ix++){
        float s = (float)ix / (SV_W-1);
        float wr = 1.f + s*(hrgb.r - 1.f);
        float wg = 1.f + s*(hrgb.g - 1.f);
        float wb = 1.f + s*(hrgb.b - 1.f);
        for(int iy=0; iy<SV_H; iy++){
            float t = (float)iy / (SV_H-1);
            float r = wr*(1.f-t);
            float g = wg*(1.f-t);
            float b = wb*(1.f-t);
            img_put(sv_img, ix, iy, make_pixel(r,g,b));
        }
    }
    sv_hue_cached = hsv.h;
    sv_dirty = 0;
}

static void build_hue_image(void) {
    if(!hue_img) hue_img = make_image(HUE_W, HUE_H);
    static const float stops[7][3]={
        {1,0,0},{1,1,0},{0,1,0},{0,1,1},{0,0,1},{1,0,1},{1,0,0}
    };
    int seg = HUE_H / 6;
    for(int s=0; s<6; s++){
        int y0 = s*seg;
        int y1 = (s==5) ? HUE_H : y0+seg;
        for(int iy=y0; iy<y1; iy++){
            float t = (float)(iy-y0)/(y1-y0);
            float r = stops[s][0] + t*(stops[s+1][0]-stops[s][0]);
            float g = stops[s][1] + t*(stops[s+1][1]-stops[s][1]);
            float b = stops[s][2] + t*(stops[s+1][2]-stops[s][2]);
            unsigned long px = make_pixel(r,g,b);
            for(int ix=0; ix<HUE_W; ix++)
                img_put(hue_img, ix, iy, px);
        }
    }
    hue_dirty = 0;
}

/* ── XColor для UI элементов (кнопки, рамки) — через XAllocColor ───────────── */
static unsigned long xrgb_alloc(float r, float g, float b) {
    XColor c;
    c.red   = (unsigned short)(r*65535);
    c.green = (unsigned short)(g*65535);
    c.blue  = (unsigned short)(b*65535);
    c.flags = DoRed|DoGreen|DoBlue;
    XAllocColor(dpy, cmap, &c);
    return c.pixel;
}
static unsigned long xhex(unsigned long h) {
    return xrgb_alloc(((h>>16)&0xff)/255.f,
                      ((h>>8) &0xff)/255.f,
                      ( h     &0xff)/255.f);
}

/* ── Sync ──────────────────────────────────────────────────────────────────── */
static void sync_hex(void) {
    RGB c = hsv2rgb(hsv);
    snprintf(hex_buf, sizeof(hex_buf), "#%02X%02X%02X",
             (int)(c.r*255+.5f),(int)(c.g*255+.5f),(int)(c.b*255+.5f));
    hex_cursor = 7;
}

static void parse_hex(void) {
    const char *s = hex_buf + (hex_buf[0]=='#' ? 1 : 0);
    if(strlen(s)!=6) return;
    for(int i=0;i<6;i++) if(!isxdigit((unsigned char)s[i])) return;
    unsigned r,g,b;
    sscanf(s,"%02x%02x%02x",&r,&g,&b);
    RGB c={r/255.f,g/255.f,b/255.f};
    hsv=rgb2hsv(c);
    sv_dirty=1;
}

/* ── Draw ──────────────────────────────────────────────────────────────────── */
static void draw_sv(void) {
    /* пересчитываем только если изменился hue */
    if(sv_dirty || sv_hue_cached != hsv.h) build_sv_image();
    XPutImage(dpy, buf, gc, sv_img, 0,0, SV_X,SV_Y, SV_W,SV_H);

    XSetForeground(dpy,gc,xhex(C_BORDER));
    XDrawRectangle(dpy,buf,gc,SV_X-1,SV_Y-1,SV_W+1,SV_H+1);

    int cx=SV_X+(int)(hsv.s*(SV_W-1));
    int cy=SV_Y+(int)((1-hsv.v)*(SV_H-1));
    XSetForeground(dpy,gc,BlackPixel(dpy,scr));
    XDrawArc(dpy,buf,gc,cx-7,cy-7,14,14,0,360*64);
    XSetForeground(dpy,gc,WhitePixel(dpy,scr));
    XDrawArc(dpy,buf,gc,cx-5,cy-5,10,10,0,360*64);
}

static void draw_hue(void) {
    if(hue_dirty) build_hue_image();
    XPutImage(dpy, buf, gc, hue_img, 0,0, HUE_X,HUE_Y, HUE_W,HUE_H);

    XSetForeground(dpy,gc,xhex(C_BORDER));
    XDrawRectangle(dpy,buf,gc,HUE_X-1,HUE_Y-1,HUE_W+1,HUE_H+1);

    int hy=HUE_Y+(int)(hsv.h/360.f*(HUE_H-1));
    int ax=HUE_X+HUE_W+4;
    XSetForeground(dpy,gc,xhex(C_TEXT));
    XPoint pts[3]={{ax+10,hy-5},{ax+10,hy+5},{ax+1,hy}};
    XFillPolygon(dpy,buf,gc,pts,3,Convex,CoordModeOrigin);
}

static void draw_swatch(void) {
    RGB c=hsv2rgb(hsv);
    XSetForeground(dpy,gc,xrgb_alloc(c.r,c.g,c.b));
    XFillRectangle(dpy,buf,gc,SWATCH_X,SWATCH_Y,SWATCH_W,SWATCH_H);
    XSetForeground(dpy,gc,xhex(C_BORDER));
    XDrawRectangle(dpy,buf,gc,SWATCH_X-1,SWATCH_Y-1,SWATCH_W+1,SWATCH_H+1);
}

static void draw_hex(void) {
    XSetForeground(dpy,gc,xhex(C_INPUT_BG));
    XFillRectangle(dpy,buf,gc,HEX_X,HEX_Y,HEX_W,HEX_H);
    XSetForeground(dpy,gc,xhex(hex_focused ? 0x8888ccu : C_BORDER));
    XDrawRectangle(dpy,buf,gc,HEX_X-1,HEX_Y-1,HEX_W+1,HEX_H+1);
    XSetForeground(dpy,gc,xhex(C_TEXT));
    if(font) XSetFont(dpy,gc,font->fid);
    int ty=HEX_Y+(HEX_H+(font?font->ascent:10))/2-1;
    XDrawString(dpy,buf,gc,HEX_X+6,ty,hex_buf,strlen(hex_buf));
    if(hex_focused){
        int cw=font?XTextWidth(font,hex_buf,hex_cursor):hex_cursor*8;
        XDrawLine(dpy,buf,gc,HEX_X+6+cw,HEX_Y+3,HEX_X+6+cw,HEX_Y+HEX_H-4);
    }
}

static void draw_btn(int x,int y,int w,int h,const char *label,unsigned long bg){
    XSetForeground(dpy,gc,xhex(bg));
    XFillRectangle(dpy,buf,gc,x,y,w,h);
    XSetForeground(dpy,gc,xhex(C_BORDER));
    XDrawRectangle(dpy,buf,gc,x-1,y-1,w+1,h+1);
    XSetForeground(dpy,gc,xhex(C_TEXT));
    if(font) XSetFont(dpy,gc,font->fid);
    int tw=font?XTextWidth(font,label,strlen(label)):(int)strlen(label)*8;
    int tx=x+(w-tw)/2;
    int ty=y+(h+(font?font->ascent:10))/2-1;
    XDrawString(dpy,buf,gc,tx,ty,label,strlen(label));
}

static void draw_ok_btn(int x,int y,int w,int h,unsigned long bg){
    XSetForeground(dpy,gc,xhex(bg));
    XFillRectangle(dpy,buf,gc,x,y,w,h);
    XSetForeground(dpy,gc,xhex(C_BORDER));
    XDrawRectangle(dpy,buf,gc,x-1,y-1,w+1,h+1);
    XSetForeground(dpy,gc,xhex(C_TEXT));
    if(font) XSetFont(dpy,gc,font->fid);
    const char *label = "OK";
    int tw  = font ? XTextWidth(font,label,strlen(label)) : 16;
    int chw = 10; /* ширина галочки */
    int gap = 5;
    int total = chw + gap + tw;
    int tx  = x + (w - total) / 2;
    int ty  = y + (h + (font ? font->ascent : 10)) / 2 - 1;
    int cy  = y + h/2;
    int cx  = tx;
    /* тонкая галочка (1px) слева от OK */
    XPoint chk[3] = {{cx, cy}, {cx+3, cy+4}, {cx+9, cy-4}};
    XDrawLines(dpy,buf,gc,chk,3,CoordModeOrigin);
    XDrawString(dpy,buf,gc,tx+chw+gap,ty,label,strlen(label));
}

static void redraw(void) {
    XSetForeground(dpy,gc,xhex(C_BG));
    XFillRectangle(dpy,buf,gc,0,0,WIN_W,WIN_H);
    draw_sv();
    draw_hue();
    draw_swatch();
    draw_hex();
    draw_btn(BTN_CANCEL_X,BTN_Y,BTN_W,BTN_H,"X Cancel",C_BTN_CANCEL);
    draw_ok_btn(BTN_OK_X, BTN_Y,BTN_W,BTN_H,C_BTN_OK);
    XCopyArea(dpy,buf,win,gc,0,0,WIN_W,WIN_H,0,0);
    XFlush(dpy);
}

/* ── Hit tests ─────────────────────────────────────────────────────────────── */
#define IN(x,y,rx,ry,rw,rh) ((x)>=(rx)&&(x)<(rx)+(rw)&&(y)>=(ry)&&(y)<(ry)+(rh))
static int in_sv(int x,int y)    {return IN(x,y,SV_X,SV_Y,SV_W,SV_H);}
/* зона клика hue — от полосы до правого края окна, чтобы ручка-стрелка тоже тянулась */
static int in_hue(int x,int y)   {return IN(x,y,HUE_X,HUE_Y,WIN_W-HUE_X,HUE_H);}
static int in_hex(int x,int y)   {return IN(x,y,HEX_X,HEX_Y,HEX_W,HEX_H);}
static int in_ok(int x,int y)    {return IN(x,y,BTN_OK_X,BTN_Y,BTN_W,BTN_H);}
static int in_cancel(int x,int y){return IN(x,y,BTN_CANCEL_X,BTN_Y,BTN_W,BTN_H);}

static void handle_sv(int x,int y){
    hsv.s=fmaxf(0,fminf(1,(float)(x-SV_X)/(SV_W-1)));
    hsv.v=fmaxf(0,fminf(1,1.f-(float)(y-SV_Y)/(SV_H-1)));
    sync_hex();
    /* sv_img не перестраиваем — hue не изменился, только курсор двигается */
}
static void handle_hue(int x,int y){
    (void)x;
    hsv.h=fmaxf(0,fminf(360,(float)(y-HUE_Y)/(HUE_H-1)*360.f));
    sv_dirty=1; /* hue изменился — SV нужно перестроить */
    sync_hex();
}

/* ── Public API ────────────────────────────────────────────────────────────── */
int run_picker(const char *init_hex, char *out_hex) {
    dpy = XOpenDisplay(NULL);
    if(!dpy){ fprintf(stderr,"Cannot open display\n"); return 0; }
    scr   = DefaultScreen(dpy);
    cmap  = DefaultColormap(dpy,scr);
    vis   = DefaultVisual(dpy,scr);
    depth = DefaultDepth(dpy,scr);
    bytes_per_pixel = (depth > 16) ? 4 : 3;

    font = XLoadQueryFont(dpy,"-*-fixed-medium-r-*-*-13-*-*-*-*-*-iso8859-1");
    if(!font) font = XLoadQueryFont(dpy,"fixed");

    if(init_hex && strlen(init_hex)==7 && init_hex[0]=='#'){
        unsigned r,g,b;
        sscanf(init_hex+1,"%02x%02x%02x",&r,&g,&b);
        RGB c={r/255.f,g/255.f,b/255.f};
        hsv=rgb2hsv(c);
        memcpy(hex_buf, init_hex, 8);
        for(int i=1;i<7;i++) hex_buf[i]=toupper(hex_buf[i]);
    } else {
        hsv=(HSV){300,1,1};
        sync_hex();
    }
    hex_cursor=7; hex_focused=0;
    sv_dirty=1; hue_dirty=1;

    win = XCreateSimpleWindow(dpy,RootWindow(dpy,scr),
                              100,100,WIN_W,WIN_H,0,
                              BlackPixel(dpy,scr),pixel_from_hex(C_BG));
    XStoreName(dpy,win,"Color Picker");

    XSizeHints *sh = XAllocSizeHints();
    sh->flags      = PMinSize;
    sh->min_width  = WIN_W;
    sh->min_height = WIN_H;
    XSetWMNormalHints(dpy,win,sh);
    XFree(sh);

    Atom wm_delete = XInternAtom(dpy,"WM_DELETE_WINDOW",False);
    XSetWMProtocols(dpy,win,&wm_delete,1);

    XSelectInput(dpy,win,ExposureMask|KeyPressMask|
                 ButtonPressMask|ButtonReleaseMask|PointerMotionMask);
    XMapWindow(dpy,win);

    gc  = XCreateGC(dpy,win,0,NULL);
    buf = XCreatePixmap(dpy,win,WIN_W,WIN_H,DefaultDepth(dpy,scr));

    int dragging=0;
    int result=0;

    XEvent ev;
    for(;;){
        /* Дросселируем MotionNotify: пропускаем промежуточные события */
        XNextEvent(dpy,&ev);

        if(ev.type == MotionNotify && dragging) {
            /* Выбрасываем все накопившиеся MotionNotify, берём последний */
            while(XCheckTypedWindowEvent(dpy,win,MotionNotify,&ev))
                ;
        }

        switch(ev.type){
        case Expose:
            /* Пропускаем дубли Expose */
            while(XCheckTypedWindowEvent(dpy,win,Expose,&ev))
                ;
            redraw();
            break;

        case ButtonPress:{
            int x=ev.xbutton.x, y=ev.xbutton.y;
            if(ev.xbutton.button==Button1){
                if(in_sv(x,y))          {dragging=1;handle_sv(x,y);redraw();}
                else if(in_hue(x,y))    {dragging=2;handle_hue(x,y);redraw();}
                else if(in_hex(x,y))    {hex_focused=1;redraw();}
                else if(in_ok(x,y))     {result=1;goto done;}
                else if(in_cancel(x,y)) {result=0;goto done;}
                else                    {hex_focused=0;redraw();}
            }
            break;}

        case ButtonRelease:
            dragging=0;
            break;

        case MotionNotify:{
            int x=ev.xmotion.x, y=ev.xmotion.y;
            if(dragging==1)      {handle_sv(x,y);redraw();}
            else if(dragging==2) {handle_hue(x,y);redraw();}
            break;}

        case KeyPress:{
            KeySym ks=XLookupKeysym(&ev.xkey,0);
            if(ks==XK_Return||ks==XK_KP_Enter){
                if(hex_focused){parse_hex();sync_hex();hex_focused=0;redraw();}
                else{result=1;goto done;}
            } else if(ks==XK_Escape){
                result=0;goto done;
            } else if(hex_focused){
                if(ks==XK_BackSpace){
                    int len=strlen(hex_buf);
                    if(hex_cursor>0){
                        memmove(hex_buf+hex_cursor-1,hex_buf+hex_cursor,
                                len-hex_cursor+1);
                        hex_cursor--;
                    }
                    parse_hex();redraw();
                } else if(ks==XK_Delete){
                    int len=strlen(hex_buf);
                    if(hex_cursor<len)
                        memmove(hex_buf+hex_cursor,hex_buf+hex_cursor+1,
                                len-hex_cursor);
                    parse_hex();redraw();
                } else if(ks==XK_Left){
                    if(hex_cursor>0) hex_cursor--;
                    redraw();
                } else if(ks==XK_Right){
                    if(hex_cursor<(int)strlen(hex_buf)) hex_cursor++;
                    redraw();
                } else {
                    char tmp[8]; int n;
                    n=XLookupString(&ev.xkey,tmp,sizeof(tmp)-1,NULL,NULL);
                    tmp[n]=0;
                    for(int i=0;i<n;i++){
                        char ch=tmp[i];
                        if(ch=='#'||isxdigit((unsigned char)ch)){
                            int len=strlen(hex_buf);
                            if(len<7){
                                memmove(hex_buf+hex_cursor+1,
                                        hex_buf+hex_cursor,
                                        len-hex_cursor+1);
                                hex_buf[hex_cursor]=toupper(ch);
                                hex_cursor++;
                            }
                        }
                    }
                    parse_hex();redraw();
                }
            }
            break;}

        case ClientMessage:
            if((Atom)ev.xclient.data.l[0]==wm_delete){result=0;goto done;}
            break;
        }
    }

done:
    if(result && out_hex){
        RGB c=hsv2rgb(hsv);
        snprintf(out_hex,8,"#%02X%02X%02X",
                 (int)(c.r*255+.5f),(int)(c.g*255+.5f),(int)(c.b*255+.5f));
    }
    if(sv_img)  { free(sv_img->data);  sv_img->data=NULL;  XDestroyImage(sv_img);  sv_img=NULL; }
    if(hue_img) { free(hue_img->data); hue_img->data=NULL; XDestroyImage(hue_img); hue_img=NULL; }
    if(font)    XFreeFont(dpy,font);
    XFreePixmap(dpy,buf);
    XFreeGC(dpy,gc);
    XDestroyWindow(dpy,win);
    XCloseDisplay(dpy);
    return result;
}
