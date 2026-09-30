package io.p2pmessenger.app;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Matrix;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.MediaStore;
import androidx.core.content.FileProvider;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

/** System photo picker, bounded decoding, and a Qt-independent result bridge. */
public final class AvatarPhotoActivity extends Activity {
    public static native void result(String path, String error);
    private int mode;
    private Uri cameraUri;
    public static void open(Context context, int mode) {
        Intent intent = new Intent(context, AvatarPhotoActivity.class);
        intent.putExtra("mode",mode);
        if (!(context instanceof Activity)) intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        if (context instanceof Activity) ((Activity)context).runOnUiThread(() -> {
            try { context.startActivity(intent); } catch (Exception e) { result("","系统选择器无法启动"); }
        });
        else context.startActivity(intent);
    }
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        mode = getIntent().getIntExtra("mode",0);
        if (state != null) { String saved=state.getString("cameraUri"); if(saved!=null)cameraUri=Uri.parse(saved); return; }
        try {
            if (mode==4) {
                File pictures=new File(getCacheDir(),"camera"); if (!pictures.exists() && !pictures.mkdirs()) throw new Exception();
                File target=File.createTempFile("photo-",".jpg",pictures);
                cameraUri=FileProvider.getUriForFile(this,getPackageName()+".qtprovider",target);
                Intent camera=new Intent(MediaStore.ACTION_IMAGE_CAPTURE); camera.putExtra(MediaStore.EXTRA_OUTPUT,cameraUri);
                camera.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                camera.setClipData(android.content.ClipData.newRawUri("photo",cameraUri));
                startActivityForResult(camera,8211); return;
            }
            Intent picker = new Intent(mode == 2 ? Intent.ACTION_OPEN_DOCUMENT : Build.VERSION.SDK_INT >= 33 ? "android.provider.action.PICK_IMAGES" : Intent.ACTION_GET_CONTENT);
            picker.setType(mode == 2 ? "*/*" : "image/*");
            if (mode == 2 || Build.VERSION.SDK_INT < 33) picker.addCategory(Intent.CATEGORY_OPENABLE);
            picker.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivityForResult(picker, 8211);
        } catch (Exception e) { result("", "无法打开系统相册，请检查系统图片选择器"); finish(); }
    }
    @Override protected void onSaveInstanceState(Bundle state) { super.onSaveInstanceState(state); if(cameraUri!=null)state.putString("cameraUri",cameraUri.toString()); }
    @Override protected void onActivityResult(int request, int code, Intent data) {
        super.onActivityResult(request,code,data);
        if (request != 8211) return;
        Uri selected=mode==4?cameraUri:data==null?null:data.getData();
        if (code != RESULT_OK || selected==null) { result("", ""); finish(); return; }
        final Uri uri=selected;
        new Thread(() -> {
            Bitmap bitmap = null;
            try {
                if (mode == 2) {
                    String name = "attachment";
                    try (android.database.Cursor cursor = getContentResolver().query(uri,new String[]{android.provider.OpenableColumns.DISPLAY_NAME},null,null,null)) {
                        if (cursor != null && cursor.moveToFirst()) name = cursor.getString(0);
                    }
                    if (name == null || name.isEmpty()) name = "attachment";
                    name = new File(name).getName().replace('/', '_').replace('\\','_');
                    File directory = new File(getCacheDir(),"picked/"+java.util.UUID.randomUUID());
                    if (!directory.mkdirs()) throw new Exception();
                    File target = new File(directory,name); long total = 0;
                    try (InputStream input = getContentResolver().openInputStream(uri); FileOutputStream output = new FileOutputStream(target)) {
                        if (input == null) throw new Exception();
                        byte[] buffer = new byte[65536]; int count;
                        while ((count = input.read(buffer)) != -1) {
                            total += count;
                            if (total > 2*1024*1024) { target.delete(); throw new Exception("辅助连接文件目前限制 2 MB，请选择较小文件"); }
                            output.write(buffer,0,count);
                        }
                    }
                    String path = target.getAbsolutePath(); runOnUiThread(() -> { result(path,""); finish(); }); return;
                }
                BitmapFactory.Options options = new BitmapFactory.Options(); options.inJustDecodeBounds = true;
                try (InputStream stream = getContentResolver().openInputStream(uri)) { BitmapFactory.decodeStream(stream,null,options); }
                if (options.outWidth <= 0 || options.outHeight <= 0 || options.outWidth > 30000 || options.outHeight > 30000) throw new Exception();
                options.inJustDecodeBounds = false; options.inSampleSize = 1;
                int bound=mode>=3?1600:256;
                while (Math.max(options.outWidth,options.outHeight) / options.inSampleSize > bound) options.inSampleSize *= 2;
                try (InputStream stream = getContentResolver().openInputStream(uri)) { bitmap = BitmapFactory.decodeStream(stream,null,options); }
                if (bitmap == null) throw new Exception();
                if (Build.VERSION.SDK_INT >= 24) {
                    try (InputStream stream = getContentResolver().openInputStream(uri)) {
                        android.media.ExifInterface exif = new android.media.ExifInterface(stream);
                        int orientation = exif.getAttributeInt(android.media.ExifInterface.TAG_ORIENTATION,1);
                        Matrix transform = new Matrix();
                        if (orientation == 6) transform.postRotate(90);
                        if (orientation == 3) transform.postRotate(180);
                        if (orientation == 8) transform.postRotate(270);
                        if (!transform.isIdentity()) { Bitmap rotated = Bitmap.createBitmap(bitmap,0,0,bitmap.getWidth(),bitmap.getHeight(),transform,true); if (rotated != bitmap) bitmap.recycle(); bitmap = rotated; }
                    } catch (Exception ignored) { /* EXIF is optional. */ }
                }
                byte[] png = null;
                int maximum=mode>=3?1900000:mode==1?190000:24000;
                for (int size : mode>=3?new int[]{1600,1280,960}:mode == 1 ? new int[]{256,192,128} : new int[]{128,96,80,64}) {
                    float scale = Math.min(1f, (float)size / Math.max(bitmap.getWidth(),bitmap.getHeight()));
                    Bitmap thumb = Bitmap.createScaledBitmap(bitmap,Math.max(1,Math.round(bitmap.getWidth()*scale)),Math.max(1,Math.round(bitmap.getHeight()*scale)),true);
                    ByteArrayOutputStream bytes = new ByteArrayOutputStream(); thumb.compress(mode>=3?Bitmap.CompressFormat.JPEG:Bitmap.CompressFormat.PNG,mode>=3?85:100,bytes);
                    if (thumb != bitmap) thumb.recycle(); png = bytes.toByteArray();
                    if (png.length <= maximum) break;
                }
                if (png == null || png.length > maximum) throw new Exception();
                File file = new File(getCacheDir(), mode>=3?"photo-picked.jpg":mode == 1 ? "sticker-picked.png" : "avatar-picked.png");
                try (FileOutputStream stream = new FileOutputStream(file)) { stream.write(png); }
                String path = file.getAbsolutePath(); runOnUiThread(() -> { result(path,""); finish(); });
            } catch (Exception | OutOfMemoryError e) { String reason = mode == 2 ? "文件读取失败（辅助连接目前限制 2 MB）" : "相册图片无法读取，请选择另一张图片"; runOnUiThread(() -> { result("",reason); finish(); }); }
            finally { if (bitmap != null) bitmap.recycle(); }
        },"avatar-thumbnail").start();
    }
}
