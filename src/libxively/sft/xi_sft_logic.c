/* Copyright (c) 2003-2017, LogMeIn, Inc. All rights reserved.
 *
 * This is part of the Xively C Client library,
 * it is licensed under the BSD 3-Clause license.
 */

#include <xi_sft_logic.h>
#include <stddef.h>
#include <xi_macros.h>
#include <xi_debug.h>

xi_state_t xi_sft_make_context( xi_sft_context_t** context,
                                const char** updateable_files,
                                uint16_t updateable_files_count,
                                fn_send_control_message fn_send_message,
                                void* user_data )
{
    if ( NULL == context )
    {
        return XI_INVALID_PARAMETER;
    }

    if ( NULL != *context )
    {
        return XI_INVALID_PARAMETER;
    }

    xi_state_t state = XI_STATE_OK;

    XI_ALLOC_AT( xi_sft_context_t, *context, state );

    ( *context )->fn_send_message        = fn_send_message;
    ( *context )->send_message_user_data = user_data;
    ( *context )->updateable_files       = updateable_files;
    ( *context )->updateable_files_count = updateable_files_count;

    return state;

err_handling:

    XI_SAFE_FREE( *context );
    return state;
}

xi_state_t xi_sft_free_context( xi_sft_context_t** context )
{
    if ( NULL != context && NULL != *context )
    {
        xi_control_message_free( &( *context )->update_message_fua );

        XI_SAFE_FREE( *context );
    }

    return XI_STATE_OK;
}

xi_state_t xi_sft_on_connected( xi_sft_context_t* context )
{
    // printf( "%s, updateable_files_count: %d\n", __FUNCTION__,
    //         context->updateable_files_count );

    if ( NULL == context )
    {
        return XI_INVALID_PARAMETER;
    }

    xi_control_message_t* message_file_info = xi_control_message_create_file_info(
        context->updateable_files, NULL /* todo_atigyi: handle revisions */,
        context->updateable_files_count );

    ( *context->fn_send_message )( context->send_message_user_data, message_file_info );

    return XI_STATE_OK;
}

xi_state_t
xi_sft_on_message( xi_sft_context_t* context, xi_control_message_t* sft_message_in )
{
    if ( NULL == context || NULL == sft_message_in )
    {
        return XI_INVALID_PARAMETER;
    }

    xi_state_t state = XI_STATE_OK;

    switch ( sft_message_in->common.msgtype )
    {
        case XI_CONTROL_MESSAGE_SC__SFT_FILE_UPDATE_AVAILABLE:
        {
            /* todo?: check whether device is ready to start download of file */

            if ( NULL != context->update_message_fua )
            {
                xi_control_message_free( &context->update_message_fua );
            }

            context->update_message_fua = sft_message_in;
            /* prevent deallocation */
            sft_message_in = NULL;

            if ( 0 < context->update_message_fua->file_update_available.list_len &&
                 NULL != context->update_message_fua->file_update_available.list )
            {
                context->update_current_file =
                    &context->update_message_fua->file_update_available.list[0];

                xi_control_message_t* message_file_get_chunk =
                    xi_control_message_create_file_get_chunk(
                        context->update_current_file->name,
                        context->update_current_file->revision, 0,
                        XI_MIN( XI_SFT_FILE_CHUNK_SIZE,
                                context->update_current_file->size_in_bytes ) );

                ( *context->fn_send_message )( context->send_message_user_data,
                                               message_file_get_chunk );
            }
        }
        break;
        case XI_CONTROL_MESSAGE_SC__SFT_FILE_CHUNK:
        {
            /*
             *  - store the chunk through file/firmware BSP
             *  - if last chunk close file, start appropriate action
             *      - if firmware: start firmware update or "just" report downloaded
             *        and wait for remote FWU trigger
             *       - other files may have their own action, no-operation is an option
             */

            /* check whether FILE_CHUNK's filename matches the requested filename */
            if ( NULL != context->update_current_file &&
                 NULL != context->update_current_file->name &&
                 NULL != sft_message_in->file_chunk.name &&
                 0 == strcmp( context->update_current_file->name,
                              sft_message_in->file_chunk.name ) )
            {
                const uint32_t all_downloaded_bytes =
                    sft_message_in->file_chunk.offset + sft_message_in->file_chunk.length;

                printf( "         === === === downloading file: %s, %d / %d, [%d%%]\n",
                        context->update_current_file->name,
                        context->update_current_file->size_in_bytes, all_downloaded_bytes,
                        ( all_downloaded_bytes * 100 ) /
                            context->update_current_file->size_in_bytes );

                /* todo_atigyi  : process the bytes in sft_message_in->file_chunk.chunk */

                if ( all_downloaded_bytes < context->update_current_file->size_in_bytes )
                {
                    /* current file is not downloaded yet, continue the download */

                    xi_control_message_t* message_file_get_chunk =
                        xi_control_message_create_file_get_chunk(
                            context->update_current_file->name,
                            context->update_current_file->revision, all_downloaded_bytes,
                            XI_MIN( XI_SFT_FILE_CHUNK_SIZE,
                                    context->update_current_file->size_in_bytes -
                                        all_downloaded_bytes /* == bytes left */ ) );

                    ( *context->fn_send_message )( context->send_message_user_data,
                                                   message_file_get_chunk );
                }
                else
                {
                    /* file downloaded, continue with next file in list */

                    {
                        xi_control_message_t* message_file_status =
                            xi_control_message_create_file_status(
                                context->update_current_file->name,
                                context->update_current_file->revision,
                                XI_CONTROL_MESSAGE__SFT_FILE_STATUS_PHASE_DOWNLOADED,
                                XI_CONTROL_MESSAGE__SFT_FILE_STATUS_CODE_SUCCESS );

                        ( *context->fn_send_message )( context->send_message_user_data,
                                                       message_file_status );

                        message_file_status = xi_control_message_create_file_status(
                            context->update_current_file->name,
                            context->update_current_file->revision,
                            XI_CONTROL_MESSAGE__SFT_FILE_STATUS_PHASE_PROCESSING,
                            XI_CONTROL_MESSAGE__SFT_FILE_STATUS_CODE_SUCCESS );

                        ( *context->fn_send_message )( context->send_message_user_data,
                                                       message_file_status );

                        message_file_status = xi_control_message_create_file_status(
                            context->update_current_file->name,
                            context->update_current_file->revision,
                            XI_CONTROL_MESSAGE__SFT_FILE_STATUS_PHASE_FINISHED,
                            XI_CONTROL_MESSAGE__SFT_FILE_STATUS_CODE_SUCCESS );

                        ( *context->fn_send_message )( context->send_message_user_data,
                                                       message_file_status );
                    }

                    context->update_current_file =
                        xi_control_message_file_update_available_get_next_file_desc_ext(
                            &context->update_message_fua->file_update_available,
                            sft_message_in->file_chunk.name );

                    if ( NULL != context->update_current_file )
                    {
                        xi_control_message_t* message_file_get_chunk =
                            xi_control_message_create_file_get_chunk(
                                context->update_current_file->name,
                                context->update_current_file->revision, 0,
                                XI_MIN( XI_SFT_FILE_CHUNK_SIZE,
                                        context->update_current_file->size_in_bytes ) );

                        ( *context->fn_send_message )( context->send_message_user_data,
                                                       message_file_get_chunk );
                    }
                    else
                    {
                        /* no further files to download, finished with download process */
                        xi_control_message_free( &context->update_message_fua );
                    }
                }
            }
            else
            {
                /* Something went wrong. Somehow the current file under update
                 * is not in sync with arrived FILE_CHUNK message. */

                xi_control_message_t* message_file_status =
                    xi_control_message_create_file_status(
                        context->update_current_file->name,
                        context->update_current_file->revision,
                        XI_CONTROL_MESSAGE__SFT_FILE_STATUS_PHASE_DOWNLOADED,
                        XI_CONTROL_MESSAGE__SFT_FILE_STATUS_CODE_ERROR__UNEXPECTED_FILE_CHUNK_ARRIVED );

                ( *context->fn_send_message )( context->send_message_user_data,
                                               message_file_status );

                xi_debug_format( "ERROR: context->update_current_file is out of sync. "
                                 "[%p] [%s]. Dropping this FILE_CHUNK message, waiting "
                                 "for the proper one...",
                                 context->update_current_file,
                                 context->update_current_file
                                     ? context->update_current_file->name
                                     : "n/a" );
            }
        }
        break;

        default:

            xi_debug_format( "WARNING: unhandled incoming SFT message with type: %d",
                             sft_message_in->common.msgtype );
    }

    xi_control_message_free( &sft_message_in );

    return state;
}