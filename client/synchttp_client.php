<?php
class synchttp
{
    function http_get($host, $port, $url, $data)
    {
        $synchttp_socket = @fsockopen($host, $port, $errno, $errstr, 5);
        if (!$synchttp_socket)
        {
            return false;
        }
        $out = "GET ${url}&data=${data} HTTP/1.1\r\n";
        $out .= "Host: ${host}\r\n";
        $out .= "Connection: close\r\n";
        $out .= "\r\n";
        fwrite($synchttp_socket, $out);
        $line = trim(fgets($synchttp_socket));
        $header = $line;
        list($proto, $rcode, $result) = explode(" ", $line);
        $len = -1;
        while (($line = trim(fgets($synchttp_socket))) != "")
        {
            $header .= $line;
            if (strstr($line, "Content-Length:"))
            {
                list($cl, $len) = explode(" ", $line);
            }
        }
        if ($len < 0)
        {
            return false;
        }
        $body = @fread($synchttp_socket, $len);
        fclose($synchttp_socket);
        return $body;
    }

    function http_post($host, $port, $url, $data)
    {
        $synchttp_socket = @fsockopen($host, $port, $errno, $errstr, 5);
        if (!$synchttp_socket)
        {
            return false;
        }
        $out = "POST ${url} HTTP/1.1\r\n";
        $out .= "Host: ${host}\r\n";
        $out .= "Content-Length: " . strlen($data) . "\r\n";
        $out .= "Connection: close\r\n";
        $out .= "\r\n";
        $out .= $data;
        fwrite($synchttp_socket, $out);
        $line = trim(fgets($synchttp_socket));
        $header = $line;
        list($proto, $rcode, $result) = explode(" ", $line);
        $len = -1;
        while (($line = trim(fgets($synchttp_socket))) != "")
        {
            $header .= $line;
            if (strstr($line, "Content-Length:"))
            {
                list($cl, $len) = explode(" ", $line);
            }
        }
        if ($len < 0)
        {
            return false;
        }
        $body = @fread($synchttp_socket, $len);
        fclose($synchttp_socket);
        return $body;
    }
}
?>
