/* Tests for ns.image.gifSize() and ns.image.jpegSize() */
var out = [];

/* gifSize — write a minimal GIF89a header and read it */
var tmp_gif = "/tmp/nsjs_test.gif";
/* GIF89a: 6-byte sig + logical screen descriptor with width=320, height=200 */
/* We write a minimal valid GIF header as a binary file is not possible via ns.file.write
   so we test the null return for a non-GIF file */
ns.file.write(tmp_gif, "NOT_A_GIF_FILE");
var g = ns.image.gifSize(tmp_gif);
out.push(g === null ? "gif-invalid:ok" : "gif-invalid:fail");
ns.file.unlink(tmp_gif);

/* gifSize — non-existent file */
var g2 = ns.image.gifSize("/tmp/nsjs_nonexistent_99999.gif");
out.push(g2 === null ? "gif-missing:ok" : "gif-missing:fail");

/* jpegSize — non-existent file */
var j = ns.image.jpegSize("/tmp/nsjs_nonexistent_99999.jpg");
out.push(j === null ? "jpeg-missing:ok" : "jpeg-missing:fail");

/* jpegSize — invalid file */
var tmp_jpg = "/tmp/nsjs_test.jpg";
ns.file.write(tmp_jpg, "NOT_A_JPEG_FILE");
var j2 = ns.image.jpegSize(tmp_jpg);
out.push(j2 === null ? "jpeg-invalid:ok" : "jpeg-invalid:fail");
ns.file.unlink(tmp_jpg);

ns.conn.write(out.join(","));
